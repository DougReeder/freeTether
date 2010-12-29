#ifndef FREETETHER_H
#define FREETETHER_H

#include <pthread.h>

typedef enum {
  REMOVED,
  ASSIGN_REQUESTED,
  ASSIGNED,
  REMOVE_REQUESTED
} IP_STATE;

typedef enum {
  STOPPED,
  START_REQUESTED,
  STARTED,
  STOP_REQUESTED
} DHCP_STATE;

typedef enum {
  DESTROYED,
  CREATE_REQUESTED,
  CREATED,
  DESTROY_REQUESTED
} IFACE_STATE;

typedef enum {
  UNBRIDGED,
  BRIDGED,
} BRIDGE_STATE;

typedef enum {
  UNKNOWN,
  UP,
  DOWN
} LINK_STATE;

struct client {
  char *mac;
  char *hostname;
  struct client *next;
};

struct wifi_ap {
  char *ssid;
  char *security;
  char *passphrase;
};

struct interface {
  char *ifname;
  IFACE_STATE iface_state;
  BRIDGE_STATE bridge_state;
  LINK_STATE link_state;
  struct wifi_ap *ap;
  char *type;
  struct client *clients;
  pthread_mutex_t mutex;
  struct interface *next;
};

#define DEFAULT_BRIDGE "bridge0"
#define DEFAULT_IP "10.1.1.11"
#define DEFAULT_SUBNET "255.255.255.0"
#define DEFAULT_POOLSTART "10.1.1.50"

struct iface_info {
  char *bridge;
  char *ip;
  char *subnet;
  char *poolstart;
  IP_STATE ip_state;
  DHCP_STATE dhcp_state;
  struct interface *ifaces;
  int num_ifaces;
  pthread_cond_t state_change;
  pthread_mutex_t mutex;
};

extern struct iface_info ifaceInfo;

int setupTmpDir();
void ip_forward_cleanup();
void *ipmon_thread(void *ptr);
void *usbgadgetmon_thread(void *ptr);

#endif
