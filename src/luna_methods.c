#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <malloc.h>
#include <syslog.h>
#include "luna_methods.h"
#include "luna_service.h"
#include "freetether.h"

// TODO: fix to store all the info in ifaceInfo, to get it back to mojo via sysInfo subscription.
// Change to a main thread state machine, and have callbacks awake the thread to indicate state change?
// I believe this is how MHS does it.  The current implementation of keeping track of states via async callbacks and hopskotch 
// is a little messy and hard to follow.
// Supposed to just track the states based on subscriptions, so make sure all subscriptions/values are handled

//TODO: Get it back to mojo in sysInfo subscription
//TODO: split out generic linked list, use for clients list too

#define DBUS_MOBILE_HOTSPOT "luna://com.palm.mobilehotspot"
#define DBUS_NETROUTE "luna://com.palm.netroute"
#define DBUS_DHCP "luna://com.palm.dhcp"

// TODO: Smooth/verify subscriptions and cancellability
// Just subscribe to them all at the beginning rather than when needed, 
// which will be better for tracking states too
struct subscription {
  LSFilterFunc callback;
  bool subscribed;
};

struct subscription dhcp_lease = {lease_callback, false};
struct subscription interface_status = {iface_status_callback, false};
struct subscription bluetooth = {bt_callback, false};
struct subscription btmonitor = {btmon_callback, false};

static char *ipstate_str() {
  switch(ifaceInfo.ip_state) {
    case REMOVED:
      return "REMOVED";
      break;
    case ASSIGN_REQUESTED:
      return "ASSIGN REQUESTED";
      break;
    case ASSIGNED:
      return "ASSIGNED";
      break;
    case REMOVE_REQUESTED:
      return "REMOVE REQUESTED";
      break;
    default:
      return "ERROR";
      break;
  }
}

static char *dhcpstate_str() {
  switch(ifaceInfo.dhcp_state) {
    case STOPPED:
      return "STOPPED";
      break;
    case START_REQUESTED:
      return "START REQUESTED";
      break;
    case STARTED:
      return "STARTED";
      break;
    case STOP_REQUESTED:
      return "STOP REQUESTED";
      break;
    default:
      return "ERROR";
      break;
  }
}

static char *ifacestate_str(struct interface *iface) {
  switch(iface->iface_state) {
    case DESTROYED:
      return "DESTROYED";
      break;
    case CREATE_REQUESTED:
      return "CREATE REQUESTED";
      break;
    case CREATED:
      return "CREATED";
      break;
    case DESTROY_REQUESTED:
      return "DESTROY REQUESTED";
      break;
  }
}

static char *bridgestate_str(struct interface *iface) {
  switch(iface->bridge_state) {
    case UNBRIDGED:
      return "UNBRIDGED";
      break;
    case BRIDGED:
      return "BRIDGED";
      break;
    default:
      return "ERROR";
      break;
  }
}

static char *linkstate_str(struct interface *iface) {
  switch(iface->link_state) {
    case UNKNOWN:
      return "UNKNOWN";
      break;
    case UP:
      return "UP";
      break;
    case DOWN:
      return "DOWN";
      break;
    default:
      return "ERROR";
      break;
  }
}

static char *generate_sysinfo_json() {
  struct interface *iface = ifaceInfo.ifaces;
  char *payload;

  asprintf(&payload,
      "\"sysinfo\": { "\
        "\"ifbridge\": \"%s\", "\
        "\"IPv4Address\": \"%s\", "\
        "\"IPv4Subnet\": \"%s\", "\
        "\"IPv4PoolStart\": \"%s\", "\
        "\"stateIPv4\": \"%s\", "\
        "\"stateDHCPServer\": \"%s\", "\
        "\"interfaces\": [",
        ifaceInfo.bridge, ifaceInfo.ip, ifaceInfo.subnet, ifaceInfo.poolstart, ipstate_str(), dhcpstate_str());

  while (iface) {
    asprintf(&payload, "%s { "\
        "\"ifname\": \"%s\", "\
        "\"stateInterface\": \"%s\", "\
        "\"stateInterfaceBridged\": \"%s\", "\
        "\"stateLink\": \"%s\", "\
        "\"type\": \"%s\", ",
        payload, iface->ifname, ifacestate_str(iface), bridgestate_str(iface), linkstate_str(iface), 
        iface->type);

    if (!strcmp(iface->type, "WiFi")) {
      asprintf(&payload, "%s \"SSID\": \"%s\", \"Security\": \"%s\"",
          payload, iface->ap->ssid, iface->ap->security);
      }

    if (iface->next)
      asprintf(&payload, "%s },", payload);
    else
      asprintf(&payload, "%s } ] }", payload);

    iface = iface->next;
  }

  return payload;
}

static void sysinfo_response() {
  LSError lserror;
  LSErrorInit(&lserror);
  char *payload;

  payload = generate_sysinfo_json();
  asprintf(&payload, "\"returnValue\": true, %s", payload);

  if (payload) {
    LSSubscriptionRespond(serviceHandle, "/sysInfo", payload, &lserror);
    free(payload);
  }
}

static void free_iface(struct interface *iface) {
  if (iface) {
    struct client *client = iface->clients;
    struct client *prev = iface->clients;
    if (iface->ifname)
      free(iface->ifname);
    if (iface->type)
      free(iface->type);
    if (iface->ap) {
      if (iface->ap->ssid)
        free(iface->ap->ssid);
      if (iface->ap->security)
        free(iface->ap->security);
      if (iface->ap->passphrase)
        free(iface->ap->passphrase);
    }
    
    while (client) {
      if (client->mac)
        free(client->mac);
      if (client->hostname)
        free(client->hostname);

      prev = client;
      client = client->next;
      free(prev);
    }

    free(iface);
  }
}

static void delete_requested_iface() {
  struct interface *iface = ifaceInfo.ifaces;

  if (iface->iface_state == CREATE_REQUESTED) {
    ifaceInfo.ifaces = iface->next;
    free_iface(iface);
    return;
  }

  while (iface->next && iface->next->iface_state != CREATE_REQUESTED) {
    iface = iface->next;
  }

  if (iface->next) {
    struct interface *pIface = iface->next;
    iface->next = pIface->next;
    free_iface(pIface);
  }
}

struct interface *get_destroy_iface() {
  struct interface *iface = ifaceInfo.ifaces;

  while (iface && iface->iface_state != DESTROY_REQUESTED) {
    iface = iface->next;
  }

  return iface;
}

struct interface *get_requested_iface() {
  struct interface *iface = ifaceInfo.ifaces;

  while (iface && iface->iface_state != CREATE_REQUESTED) {
    iface = iface->next;
  }

  return iface;
}

struct interface *get_iface(char *ifname, char *type) {
  struct interface *iface = ifaceInfo.ifaces;

  if (!ifname && !type)
    return NULL;

  while (iface) {
    if (ifname && type) {
      if (!strcmp(iface->ifname, ifname) && !strcmp(iface->type, type))
        break;
    }
    else if (ifname) {
      if (!strcmp(iface->ifname, ifname))
        break;
    }
    else {
      if (!strcmp(iface->type, type))
        break;
    }
    iface = iface->next;
  }

  return iface;
}

void add_client(struct interface *iface, char *mac, char *hostname) {
  struct client *iter = NULL;
  struct client *client = NULL;

  if (!iface || !mac) {
    syslog(LOG_DEBUG, "Error add_client called with iface %p, mac %p\n", iface, mac);
    return;
  }

  client = calloc(1, sizeof (struct client));
  if (!client) {
    syslog(LOG_ERR, "ERROR: Unable to allocate memory for client!");
    return;
  }

  iter = iface->clients;
  if (!iter) {
    iface->clients = client;
  }
  else {
    while (iter->next) {
      iter = iter->next;
    }
    iter->next = client;
  }
}

void free_clients(struct interface *iface) {
  struct client *client = iface->clients;
  struct client *prev;
  while (client) {
    if (client->mac)
      free(client->mac);
    if (client->hostname)
      free(client->hostname);

    prev = client;
    client = client->next;
    free(prev);
  }
}

void update_clients(struct interface *iface, json_t *object) {
  if (object && object->child && object->child->type == JSON_ARRAY && object->child->child) {
    json_t *client = object->child->child;

    // Just free and regenerate, make life easier
    free_clients(iface);

    while (client) {
      char *mac, *hostname = NULL;
      json_t *wifi_client = json_find_first_label(client, "clientInfo");

      if (wifi_client && wifi_client->child) {
        json_get_string(wifi_client->child, "macAddress", &mac);
      }
      else {
        json_get_string(client, "macAddress", &mac);
        json_get_string(client, "hostName", &hostname);
      }

      add_client(iface, mac, hostname);
    }
  }
}

bool lease_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  bool returnValue = false;
  json_t *leases;

  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_bool(object, "returnValue", &returnValue);

  if (returnValue) {
    if (!dhcp_lease.subscribed)
      leases = json_find_first_label(object, "interfaceList");
    else
      leases = json_find_first_label(object, "leases");

    if (LSMessageIsSubscription(msg))
      dhcp_lease.subscribed = true;

    // leases first child should be an array.  
    // If that array has at least one child then toggle ip forward on
    if (leases && leases->child && leases->child->child) {
      if (!get_ip_forward_state())
        toggle_ip_forward_state();
    }
  }
  else {
    // if (LSMessageIsSubscription(msg))
    dhcp_lease.subscribed = false;
    ifaceInfo.dhcp_state = STOPPED;
  }

  return true;
}


bool remove_netif_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  bool ret = FALSE;

  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_bool(object, "returnValue", &ret);

  if (ret) {
    pthread_mutex_lock(&ifaceInfo.mutex);
    if (ifaceInfo.ip_state == REMOVE_REQUESTED)
      ifaceInfo.ip_state = REMOVED;
    pthread_mutex_unlock(&ifaceInfo.mutex);
  }

  return true;
}

bool dhcp_final_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  bool ret = FALSE;

  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_bool(object, "returnValue", &ret);

  if (ret) {
    pthread_mutex_lock(&ifaceInfo.mutex);
    ifaceInfo.dhcp_state = STOPPED;
    pthread_mutex_unlock(&ifaceInfo.mutex);

    if (ifaceInfo.ip_state != REMOVED) {
      char *payload = NULL;

      asprintf(&payload, "{\"ifName\": \"%s\"}", ifaceInfo.bridge);
      if (!payload)
        return false;

      pthread_mutex_lock(&ifaceInfo.mutex);
      ifaceInfo.ip_state = REMOVE_REQUESTED;
      pthread_mutex_unlock(&ifaceInfo.mutex);

      LSCall(priv_serviceHandle, "palm://com.palm.netroute/removeNetIf", payload, 
          remove_netif_callback, NULL, NULL, &lserror);

      free(payload);
    }
  }
  else {
    /* Fix the handling of return false everywhere */
  }

  return true;
}

bool dhcp_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  bool ret = FALSE;

  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_bool(object, "returnValue", &ret);

  if (ret && ifaceInfo.dhcp_state == START_REQUESTED) {
    pthread_mutex_lock(&ifaceInfo.mutex);
    ifaceInfo.dhcp_state = STARTED;
    pthread_mutex_unlock(&ifaceInfo.mutex);

    if (!dhcp_lease.subscribed)
      LS_PRIV_SUBSCRIBE("dhcp/server/leaseInformation", dhcp_lease.callback);
  }
}

bool netif_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  bool ret = FALSE;

  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_bool(object, "returnValue", &ret);

  if (ret && ifaceInfo.ip_state == ASSIGN_REQUESTED) {
    pthread_mutex_lock(&ifaceInfo.mutex);
    ifaceInfo.ip_state = ASSIGNED;
    if (ifaceInfo.dhcp_state != STARTED)
      ifaceInfo.dhcp_state = START_REQUESTED;
    pthread_mutex_unlock(&ifaceInfo.mutex);

    // TODO: less hard coding
    if (ifaceInfo.dhcp_state == START_REQUESTED) {
      LSCall(priv_serviceHandle, "luna://com.palm.dhcp/interfaceInitialize", "{ " \
            "\"interface\":\"bridge0\", "\
            "\"mode\":\"server\", "\
            "\"ipv4Address\":\"10.1.1.11\", "\
            "\"ipv4Subnet\":\"255.255.255.0\", "\
            "\"ipv4Router\":\"10.1.1.11\", "\
            "\"dnsServers\":[\"10.1.1.11\"], "\
            "\"ipv4RangeStart\":\"10.1.1.50\", "\
            "\"maxLeases\":15, "\
            "\"leaseTime\":7200}", 
            dhcp_callback, NULL, NULL, &lserror);
    }
  }

  return true;
}

void add_bridge(struct interface *iface) {
  LSError lserror;
  LSErrorInit(&lserror);
  int ret;

  // TODO: Check that bridge/interface doesn't already exist
  ret = br_add_bridge("bridge0");
  syslog(LOG_DEBUG, "add bridge %d\n", ret);
  ret = br_add_interface("bridge0", iface->ifname);
  syslog(LOG_DEBUG, "add interface %d\n", ret);

  system("brctl addbr bridge0");
  char command[80];
  sprintf(command, "brctl addif bridge0 %s", iface->ifname);
  system(command);

  // Have to up at least for uap0 it seems
  sprintf(command, "ifconfig %s up", iface->ifname);
  system(command);

  pthread_mutex_lock(&iface->mutex);
  iface->bridge_state = BRIDGED;
  pthread_mutex_unlock(&iface->mutex);

  if (ifaceInfo.ip_state != ASSIGNED) {
    pthread_mutex_lock(&ifaceInfo.mutex);
    ifaceInfo.ip_state = ASSIGN_REQUESTED;
    pthread_mutex_unlock(&ifaceInfo.mutex);

    // TODO: less hard coding
    LSCall(priv_serviceHandle, "luna://com.palm.netroute/addNetIf", 
      "{ " \
        "\"ifName\": \"bridge0\", " \
        "\"networkUsage\": [\"private\"], " \
        "\"networkTechnology\": \"unknown\", " \
        "\"networkScope\": \"lan\", " \
        "\"ipv4\": " \
        "{ " \
            "\"ip\": \"0x0b01010a\", " \
            "\"netmask\": \"0x00ffffff\", " \
            "\"gateway\": \"0x0b01010a\" " \
        "}" \
        "}",
        netif_callback, NULL, NULL, &lserror);
  }
}

bool iface_status_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  bool returnValue = false;
  char *ifname = NULL;
  char *state = NULL;
  struct interface *iface;
  LINK_STATE link_state = UNKNOWN;
  json_t *object;

  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_bool(object, "returnValue", &returnValue);

  if (returnValue) {
    if (LSMessageIsSubscription(msg))
      interface_status.subscribed = true;
    json_get_string(object, "ifname", &ifname);
    json_get_string(object, "state", &state);
    if (ifname && state) {
      if (!strcmp(state, "down"))
        link_state = DOWN;
      if (!strcmp(state, "up"))
        link_state = UP;

      iface = get_iface(ifname, NULL);
      pthread_mutex_lock(&iface->mutex);
      iface->link_state = link_state;
      pthread_mutex_unlock(&iface->mutex);
      syslog(LOG_DEBUG, "link_state %d", link_state);
      syslog(LOG_DEBUG, "bridge_state %d", iface->bridge_state);
      if (link_state == UP && iface->bridge_state != BRIDGED)
        add_bridge(iface);
      if (link_state == DOWN)
        iface->bridge_state = UNBRIDGED;

      update_clients(iface, json_find_first_label(object, "clientList"));
    }
  }
  else {
    // if (LSMessageIsSubscription(msg))
    interface_status.subscribed = false;
  }

  return true;
}

void remove_iface(struct interface *iface) {
  struct interface *iter = ifaceInfo.ifaces;

  if (iter == iface) {
    ifaceInfo.ifaces = iface->next;
    free_iface(iface);
    return;
  }

  while (iter->next && iface != iter->next) {
    iter = iter->next;
  }

  if (iter->next) {
    iter->next = iface->next;
    free_iface(iface);
  }
}

int num_interfaces() {
  struct interface *iface = ifaceInfo.ifaces;
  int count = 0;

  while (iface) {
    iface = iface->next;
    count++;
  }

  return count;
}

bool delete_ap_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  struct interface *iface = NULL;
  bool ret = false;
  char *ifname = NULL;

  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_bool(object, "returnValue", &ret);

  if (ret && iface->iface_state == DESTROY_REQUESTED) {
    iface = get_destroy_iface();
    if (iface) {
      if (!strcmp(iface->type, "wifi")) {
        LSCall(priv_serviceHandle, "luna://com.palm.wifi/setstate", "{\"state\":\"enabled\"}", 
            NULL, NULL, NULL, &lserror);
      }

      remove_iface(iface);
      if (!num_interfaces()) {
        char *payload = NULL;

        asprintf(&payload, "{\"interface\": \"%s\"}", ifaceInfo.bridge);
        if (!payload)
          return false;

        if (ifaceInfo.dhcp_state != STOPPED) {
          pthread_mutex_lock(&ifaceInfo.mutex);
          ifaceInfo.dhcp_state = STOP_REQUESTED;
          pthread_mutex_unlock(&ifaceInfo.mutex);
          LSCall(priv_serviceHandle, "palm://com.palm.dhcp/interfaceFinalize", payload, 
              dhcp_final_callback, NULL, NULL, &lserror);
        }

        free(payload);
      }
    }
  }

  return true;
}

bool create_ap_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  struct interface *iface = NULL;
  bool ret = false;
  char *ifname = NULL;

  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_bool(object, "returnValue", &ret);

  if (ret) {
    json_get_string(object, "ifname", &ifname);
    iface = get_requested_iface();

    if (iface && ifname) {
      pthread_mutex_lock(&ifaceInfo.ifaces[0].mutex);
      iface->ifname = strdup(ifname);
      iface->iface_state = CREATED;
      pthread_mutex_unlock(&ifaceInfo.ifaces[0].mutex);
      if (!interface_status.subscribed)
        LS_PRIV_SUBSCRIBE("wifi/interfaceStatus", interface_status.callback);
    }
  }
  else {
    delete_requested_iface();
  }

  return true;
}

int remove_interface(char *type, char *ifname) {
  LSError lserror;
  LSErrorInit(&lserror);
  char *payload = NULL;
  struct interface *iface;

  iface = get_iface(type, ifname);
  if (!iface)
    return -1;

  asprintf(&payload, "{\"ifname\": \"%s\"}", iface->ifname);
  if (!payload)
    return -1;

  pthread_mutex_lock(&iface->mutex);
  iface->iface_state = DESTROY_REQUESTED;
  pthread_mutex_unlock(&iface->mutex);

  LSCall(priv_serviceHandle, "luna://com.palm.wifi/deleteAP", payload, delete_ap_callback, 
      NULL, NULL, &lserror);

  free(payload);

  return true;
}

bool interfaceRemove(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  char *type = NULL;
  char *ifname = NULL;

  object = json_parse_document(LSMessageGetPayload(msg));
  type = object->child->text;

  if (!type) {
    LS_REPLY_ERROR("No Interface type specified");
    return true;
  }

  json_t *type_label = json_find_first_label(object, type);
  json_get_string(type_label->child, "ifname", &ifname);

  remove_interface(type, ifname);
  if (!strcmp(type, "wifi")) {
    LSCall(priv_serviceHandle, "palm://com.palm.wifi/deleteAP", "{\"ifname\":\"uap0\"}", 
        NULL, NULL, NULL, &lserror);
    LSCall(priv_serviceHandle, "palm://com.palm.wifi/setstate", "{\"state\":\"enabled\"}", 
        NULL, NULL, NULL, &lserror);
  }

  // TODO: Only finalize and remove if there are no additional interfaces active
  LSCall(priv_serviceHandle, "palm://com.palm.dhcp/interfaceFinalize", "{\"interface\":\"bridge0\"}", NULL, NULL, NULL, &lserror);
  LSCall(priv_serviceHandle, "palm://com.palm.netroute/removeNetIf", "{\"ifName\":\"bridge0\"}", NULL, NULL, NULL, &lserror);

  LSMessageReply(sh, msg, "{\"returnValue\":true}", &lserror);

  return true;
}

struct interface *request_interface(char *type, char *ifname, struct wifi_ap *ap) {
  LSError lserror;
  LSErrorInit(&lserror);
  struct interface *iface = NULL;
  struct interface *iter = NULL;
  char *payload = NULL;

  if (ifname && get_iface(ifname, NULL)) {
    syslog(LOG_WARNING, "WARNING: Already requested interface %s, not setting up again\n", ifname);
    return NULL;
  }

  iface = calloc(1, sizeof(*iface));
  if (!iface) {
    syslog(LOG_ERR, "ERROR: Unable to allocate space for additional interface!");
    return NULL;
  }

  iter = ifaceInfo.ifaces;
  if (!iter) {
    ifaceInfo.ifaces = iface;
  }
  else {
    while (iter->next) {
      iter = iter->next;
    }
    iter->next = iface;
  }

  pthread_mutex_init(&iface->mutex, NULL);
  pthread_mutex_lock(&iface->mutex);
  iface->iface_state = CREATE_REQUESTED;

  if (type)
    iface->type = strdup(type);
  if (ifname)
    iface->ifname = strdup(ifname);
  if (ap)
    iface->ap = ap;

  pthread_mutex_unlock(&iface->mutex);

  if (!strcmp(type, "WiFi")) {
    asprintf(&payload, "{\"SSID\": \"%s\", \"Security\": \"%s\"", ap->ssid, ap->security);

    if (ap->passphrase /* && not Open ? */)
      asprintf(&payload, "%s, \"Passphrase\": %s", payload, ap->passphrase);

    asprintf(&payload, "%s}", payload);

    if (!payload)
      return NULL;

    LSCall(priv_serviceHandle, "palm://com.palm.wifi/createAP", payload, 
        create_ap_callback, NULL, NULL, &lserror);
  }

  free(payload);
  return iface;
}

bool btmon_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  bool returnValue = false;

  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_bool(object, "returnValue", &returnValue);

  if (returnValue) {
    char *radio;

    if (LSMessageIsSubscription(msg))
      btmonitor.subscribed = true;

    json_get_string(object, "radio", &radio);
    if (radio && !strcmp(radio, "on") && !bluetooth.subscribed) {
      LS_PRIV_SUBSCRIBE("bluetooth/pan/subscribenotifications", bluetooth.callback);
    }
    else {
      char *notification;

      json_get_string(object, "notification", &notification);
      if (notification && !strcmp(notification, "notifnradioon") && !bluetooth.subscribed)
        LS_PRIV_SUBSCRIBE("bluetooth/pan/subscribenotifications", bluetooth.callback);
    }

  }
  else {
    // if (LSMessageIsSubscription(msg))
    btmonitor.subscribed = false;
  }

  return true;
}

bool disable_wifi_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  bool returnValue = false;
  struct wifi_ap *ap;
  
  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_bool(object, "returnValue", &returnValue);

  if (returnValue) {
    ap = (struct wifi_ap *)ctx;
    request_interface("WiFi", NULL, ap);
  }
  
  return true;
}

bool bt_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  char *ifname = NULL;
  char *status = NULL;
  LINK_STATE link_state = UNKNOWN;

  bluetooth.subscribed = true;
  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_string(object, "ifname", &ifname);
  json_get_string(object, "status", &status);
  // Retrieve from json clientList: [macAddress: , hostName: ]

  if (ifname && status) {
    if (!strcmp(status, "connected")) {
      struct interface *iface;
      json_t *clientlist = json_find_first_label(object, "clientList");

      link_state = UP;
      iface = request_interface("bluetooth", ifname, NULL);
      if (link_state == UP && iface->bridge_state != BRIDGED)
        add_bridge(iface);

      if (clientlist && clientlist->child && clientlist->child->child) {
        char *mac;

        // TODO: fix to build client list of mac/hostnames to report
        json_get_string(clientlist->child->child, "macAddress", &mac);
        syslog(LOG_DEBUG, "first client mac %s\n", mac);
      }
    }
    else {
      link_state = DOWN;
      // TODO: clean up connections?
    }
  }

  return true;
}

bool interfaceAdd(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  char *type = NULL;
  char *ifname = NULL;
  char command[80];
  json_t *object;
  char *uri = NULL;

  // payload's first label is the type which can be wifi, bluetooth or usb
  object = json_parse_document(LSMessageGetPayload(msg));
  type = object->child->text;
  if (!type) {
    LS_REPLY_ERROR("No Interface type specified");
    return true;
  }

  if (!strcmp(type, "wifi")) {
    struct wifi_ap *ap;
    char *ssid = NULL;
    char *security = NULL;
    char *passphrase = NULL;
    char *payload = NULL;
    json_t *wifi = json_find_first_label(object, "wifi");

    json_get_string(wifi->child, "SSID", &ssid);
    json_get_string(wifi->child, "Security", &security);
    json_get_string(wifi->child, "Passphrase", &passphrase);

    if (!ssid) {
      LS_REPLY_ERROR("No SSID supplied");
      return true;
    }

    // TODO: Add check for valid security method
    if (!security) {
      security = malloc(strlen("Open") + 1);
      strcpy(security, "Open");
    }

    ap = malloc(sizeof(struct wifi_ap));
    if (!ap) {
      LS_REPLY_ERROR("Out of memory");
      return true;
    }

    if (ssid)
      ap->ssid = strdup(ssid);
    if (security)
      ap->security = strdup(security);
    if (passphrase)
      ap->passphrase = strdup(passphrase);

    LSCall(priv_serviceHandle, "palm://com.palm.wifi/setstate", "{\"state\": \"disabled\"}", 
        disable_wifi_callback, (void*)ap, NULL, &lserror);
  }
  else if (!strcmp(type, "bluetooth")) {
    if (!btmonitor.subscribed)
      LS_PRIV_SUBSCRIBE("btmonitor/monitor/subscribenotifications", btmonitor.callback);
  }
  else if (!strcmp(type, "usb")) {
    json_get_string(object, "ifname", &ifname);
    if (!ifname) {
      LS_REPLY_ERROR("Invalid ifname specified");
      return true;
    }

    request_interface("usb", ifname, NULL);
  }
  else {
    LS_REPLY_ERROR("Invalid Interface type specified");
    return true;
  }

  LSMessageReply(sh, msg, "{\"returnValue\":true}", &lserror);
  return true;
}

bool sysInfo(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  char *payload;
  bool subscribed = false;

  payload = generate_sysinfo_json();
  asprintf(&payload, "\"returnValue\": true, \"subscribed\": true, %s", payload);

  LSSubscriptionProcess(sh, msg, &subscribed, &lserror);

  if (payload) {
    LSMessageReply(sh, msg, payload, &lserror);
    free(payload);
  }
  else {
    LSMessageReply(sh, msg, "{\"returnValue\": false}", &lserror);
  }

  if (LSErrorIsSet(&lserror)) {
    LSErrorPrint(&lserror, stderr);
    LSErrorFree(&lserror);
  }

  return true;
}

bool version(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  LSMessageReply(sh, msg, "{\"returnValue:\": true}", &lserror);
  return true;
}

LSMethod luna_methods[] = {
  {"get_ip_forward", get_ip_forward},
  {"toggle_ip_forward", toggle_ip_forward},
  {"version", version},
  {"interfaceAdd", interfaceAdd},
  {"interfaceRemove", interfaceRemove},
  {"sysInfo", sysInfo},
#if 0
  {"clientList", clientList},
  {"configRead", configRead},
  {"configWrite", configWrite},
  {"deauthclient", deauthclient},
#endif
  { NULL, NULL},
};

bool register_methods(LSPalmService *serviceHandle, LSError lserror) {
	return LSPalmServiceRegisterCategory(serviceHandle, "/", luna_methods,
			NULL, NULL, NULL, &lserror);
}
