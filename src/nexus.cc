#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>

#include "common.h"
#include "nexus.h"
#include "rpc.h"

namespace ERpc {

Nexus::Nexus(uint16_t global_udp_port) : global_udp_port(global_udp_port) {
  int ret = gethostname(hostname, kMaxHostnameLen);
  if (ret == -1) {
    fprintf(stderr, "eRPC Nexus: FATAL. gethostname failed. Error = %s\n",
            strerror(errno));
    exit(-1);
  }

  erpc_dprintf("eRPC: Nexus created with global UDP port %u, hostname %s\n",
               global_udp_port, hostname);
  nexus_object = this;
  install_sigio_handler();
}

Nexus::~Nexus() {}

void Nexus::register_hook(SessionMgmtHook *hook) {
  assert(hook != nullptr);

  nexus_lock.lock();

  /* This hook must not exist */
  if (std::find(reg_hooks.begin(), reg_hooks.end(), hook) != reg_hooks.end()) {
    fprintf(stderr, "eRPC Nexus: FATAL attempt to re-register hook %p\n", hook);
    exit(-1);
  }

  /* There should be no existing hook with the same thread ID */
  for (auto reg_hook : reg_hooks) {
    if (reg_hook->app_tid == hook->app_tid) {
      fprintf(stderr,
              "eRPC Nexus: FATAL attempt to register hook with "
              "existing thread ID %d\n",
              hook->app_tid);
      exit(-1);
    }
  }

  reg_hooks.push_back(hook);

  nexus_lock.unlock();
}

void Nexus::unregister_hook(SessionMgmtHook *hook) {
  assert(hook != nullptr);

  nexus_lock.lock();

  /* The hook must exist in the vector of registered hooks */
  if (std::find(reg_hooks.begin(), reg_hooks.end(), hook) == reg_hooks.end()) {
    fprintf(stderr,
            "eRPC Nexus: FATAL attempt to unregister non-existent hook %p\n",
            hook);
    exit(-1);
  }

  reg_hooks.erase(std::remove(reg_hooks.begin(), reg_hooks.end(), hook),
                  reg_hooks.end());

  nexus_lock.unlock();
}

void Nexus::install_sigio_handler() {
  /*
   * Create a UDP socket.
   * AF_INET = IPv4, SOCK_DGRAM = datagrams, IPPROTO_UDP = datagrams over UDP.
   * Returns a file descriptor.
   */
  nexus_sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (nexus_sock_fd < 0) {
    perror("Error opening datagram socket");
    exit(1);
  }

  /*
   * Bind the socket to accept packets destined to any IP interface of this
   * machine (INADDR_ANY), and to port @udp_port.
   */
  struct sockaddr_in server;
  memset(&server, 0, sizeof(server));
  server.sin_family = AF_INET;
  server.sin_addr.s_addr = INADDR_ANY;
  server.sin_port = htons(global_udp_port);

  if (bind(nexus_sock_fd, (struct sockaddr *)&server,
           sizeof(struct sockaddr_in)) < 0) {
    perror("Error binding datagram socket");
    exit(1);
  }

  /* Set file flags. Allow receipt of asynchronous I/O signals */
  if (fcntl(nexus_sock_fd, F_SETFL, O_ASYNC | O_NONBLOCK) < 0) {
    perror("Error: fcntl F_SETFL, FASYNC");
    exit(1);
  }

  /* Ensure that only the thread that creates the Nexus receives SIGIO */
  struct f_owner_ex owner_thread;
  owner_thread.type = F_OWNER_TID;
  owner_thread.pid = (int)syscall(SYS_gettid);

  if (fcntl(nexus_sock_fd, F_SETOWN_EX, &owner_thread) < 0) {
    perror("Error: fcntl F_SETOWN_EX");
    exit(1);
  }

  /*
   * Set up a SIGIO signal handler **after** fixing the thread that will
   * receive this signal. The sigaction man page specifies that calling
   * sigaction without SA_NODEFER set ensures that the signal handler won't
   * be interrupted by the same signal while it is running. It may be
   * interrupted by other signals, however.
   */
  struct sigaction act;
  memset((void *)&act, 0, sizeof(act));
  act.sa_handler = &sigio_handler;
  if (sigaction(SIGIO, &act, nullptr) < 0) { /* Old signal handler is NULL */
    perror("sigaction");
    exit(-1);
  }
}

void Nexus::session_mgnt_handler() {
  printf("eRPC Nexus: Number of registered RPC hooks = %zu\n",
         reg_hooks.size());
}

}  // End ERpc