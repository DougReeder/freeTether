#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <sys/mount.h>

#include "freetether.h"

#ifndef APP_ID
#error Must define APP_ID macro
#endif

#ifndef DEBUG
#define DEBUG 0
#endif

struct iface_info ifaceInfo;

static void sys_info_init() {
  memset(&ifaceInfo, 0, sizeof(ifaceInfo));
  pthread_mutex_init(&ifaceInfo.mutex, NULL); 
  pthread_cond_init (&ifaceInfo.state_change, NULL);

  ifaceInfo.bridge = malloc(strlen(DEFAULT_BRIDGE) + 1);
  strcpy(ifaceInfo.bridge, DEFAULT_BRIDGE);
  ifaceInfo.ip = malloc(strlen(DEFAULT_IP) + 1);
  strcpy(ifaceInfo.ip, DEFAULT_IP);
  ifaceInfo.subnet = malloc(strlen(DEFAULT_SUBNET) + 1);
  strcpy(ifaceInfo.subnet, DEFAULT_SUBNET);
  ifaceInfo.poolstart = malloc(strlen(DEFAULT_POOLSTART) + 1);
  strcpy(ifaceInfo.poolstart, DEFAULT_POOLSTART);
}

static void *iface_thread(void *arg) {
  while (1) {
    pthread_cond_wait(&ifaceInfo.state_change, &ifaceInfo.mutex);
    pthread_mutex_unlock(&ifaceInfo.mutex);
  }

  return NULL;
}

void cleanup() {
  umount(tmpDir);
  rmdir(tmpDir);
  umount(IP_FORWARD);
  free(tmpDir);
  free(tmpIPforwardPath);
}

void sighandler(int sig) {
  cleanup();
  exit(0);
}

int setupTmpDir() {

  char template[] = "/tmp/freeTether.XXXXXX";
  char *d = mkdtemp(template);

  if (d==NULL) {
    if (DEBUG) syslog(LOG_ERR, "Failed creating tmp directory %s", template);
    return 1;
  } else {
    tmpDir = strdup(d);
    syslog(LOG_INFO, "Temporary directory %s created", tmpDir);
    if (mount("/dev/null", IP_FORWARD, NULL, MS_BIND, NULL)) {
      if (DEBUG) syslog(LOG_ERR, "Failed binding %s to %s", tmpIPforwardPath, IP_FORWARD);
      return 1;
    } else {
      syslog(LOG_INFO, "Procfs now available at %s", tmpDir);
      tmpIPforwardPath = 0;
      if (asprintf(&tmpIPforwardPath,"%s/sys/net/ipv4/ip_forward",tmpDir) == -1) {
        if (DEBUG) syslog(LOG_ERR, "Failed creating tmp ip_forward path");
        return 1;
      }
    }
  }

  return 0;

}

int main(int argc, char **argv) {
  pthread_t tid;

  sys_info_init();
  pthread_create(&tid, NULL, iface_thread, NULL);

  signal(SIGINT, sighandler);
  signal(SIGTERM, sighandler);
  signal(SIGQUIT, sighandler);
  signal(SIGHUP, sighandler);
  signal(SIGKILL, sighandler);

  openlog(APP_ID, LOG_PID, LOG_USER);

  setupTmpDir();

  if (luna_service_initialize(APP_ID))
    luna_service_start();

  return 0;
}
