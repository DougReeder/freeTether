#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>

#include "freetether.h"
#include "luna_methods.h"

#ifndef APP_ID
#error Must define APP_ID macro
#endif

struct iface_info ifaceInfo;

static int sys_info_init() {
  memset(&ifaceInfo, 0, sizeof(ifaceInfo));
  pthread_mutex_init(&ifaceInfo.mutex, NULL);
  pthread_cond_init(&ifaceInfo.state_change, NULL);
  return 0;
}

static void *iface_thread(void *arg) {
  while (1) {
    pthread_cond_wait(&ifaceInfo.state_change, &ifaceInfo.mutex);
    pthread_mutex_unlock(&ifaceInfo.mutex);
  }

  return NULL;
}

void cleanup() {
  ip_forward_cleanup();
}

void sighandler(int sig) {
  cleanup();
  exit(0);
}

int main(int argc, char **argv) {

  signal(SIGINT, sighandler);
  signal(SIGTERM, sighandler);
  signal(SIGQUIT, sighandler);
  signal(SIGHUP, sighandler);
  signal(SIGKILL, sighandler);

  openlog(APP_ID, LOG_PID, LOG_USER);

  pthread_t iface_tid;
  pthread_t ipmon_tid;

  if (!setupTmpDir() && !sys_info_init() && luna_service_initialize(APP_ID)) {
    pthread_create(&iface_tid, NULL, iface_thread, NULL);
    pthread_create(&ipmon_tid, NULL, ipmon_thread, NULL);
    luna_service_start();
  }

  return 0;
}
