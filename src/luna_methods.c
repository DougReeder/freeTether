#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <malloc.h>
#include <syslog.h>
#include "luna_methods.h"
#include "luna_service.h"
#include "freetether.h"

#define DBUS_MOBILE_HOTSPOT "luna://com.palm.mobilehotspot"
#define DBUS_NETROUTE "luna://com.palm.netroute"
#define DBUS_DHCP "luna://com.palm.dhcp"

bool hotspot_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  LSMessageRespond((LSMessage *)ctx, LSMessageGetPayload(msg), &lserror);

  if (!LSMessageIsSubscription((LSMessage *)ctx))
    LSMessageUnref((LSMessage *)ctx);

  return true;
}

void hotspot_relay(LSMessage *msg, char *method) {
  LSError lserror;
  LSErrorInit(&lserror);
  LSMessageRef(msg);
  char *uri = malloc(strlen(DBUS_MOBILE_HOTSPOT) + strlen("/") + strlen(method) + 1);

  sprintf(uri, "%s/%s", DBUS_MOBILE_HOTSPOT, method);
  LSCall(priv_serviceHandle, uri, LSMessageGetPayload(msg), hotspot_callback, (void *)msg, NULL, &lserror);
}

bool sysInfo(LSHandle *sh, LSMessage *msg, void *ctx) {
  hotspot_relay(msg, "sysInfo");
  return true;
}

bool clientList(LSHandle *sh, LSMessage *msg, void *ctx) {
  hotspot_relay(msg, "clientList");
  return true;
}

bool configRead(LSHandle *sh, LSMessage *msg, void *ctx) {
  hotspot_relay(msg, "configRead");
  return true;
}

bool configWrite(LSHandle *sh, LSMessage *msg, void *ctx) {
  hotspot_relay(msg, "configWrite");
  return true;
}

bool deauthclient(LSHandle *sh, LSMessage *msg, void *ctx) {
  hotspot_relay(msg, "deauthclient");
  return true;
}

bool version(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  LSMessageReply(sh, msg, "{\"returnValue:\": true}", &lserror);
  return true;
}

struct interface *get_requested_iface() {
  struct interface *iface = ifaceInfo.ifaces;

  while (iface && iface->iface_state != CREATE_REQUESTED) {
    iface = iface->next;
  }

  return iface;
}

struct interface *get_iface(char *ifname) {
  struct interface *iface = ifaceInfo.ifaces;

  if (!ifname)
    return NULL;

  while (iface && strcmp(iface->ifname, ifname)) {
    iface = iface->next;
  }

  return iface;
}

bool iface_status_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  bool ret = false;
  char *ifname = NULL;
  char *state = NULL;
  struct interface *iface;
  IFACE_STATE iface_state = UNKNOWN;
  json_t *object;

  object = json_parse_document(LSMessageGetPayload(msg));
  json_get_bool(object, "returnValue", &ret);

  if (ret) {
    json_get_string(object, "ifname", &ifname);
    json_get_string(object, "state", &state);
    if (ifname && state) {
      if (!strcmp(state, "down"))
        iface_state = DOWN;
      if (!strcmp(state, "up"))
        iface_state = UP;

      iface = get_iface(ifname);
      pthread_mutex_lock(&iface->mutex);
      iface->iface_state = iface_state;
      pthread_mutex_unlock(&iface->mutex);
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
    json_get_string(object, "returnValue", &ifname);
    iface = get_requested_iface();
    if (iface) {
      pthread_mutex_lock(&ifaceInfo.ifaces[0].mutex);
      iface->ifname = malloc(strlen(ifname) + 1);
      strcpy(iface->ifname, ifname);
      iface->iface_state = CREATED;
      pthread_mutex_unlock(&ifaceInfo.ifaces[0].mutex);
      LS_PRIV_SUBSCRIBE("wifi/interfaceStatus", iface_status_callback);
    }
  }

  return true;
}

// Hey man, what kind of hogwash crap stuffed together code is this?
// it's 3 AM leave me alone!

bool interfaceRemove(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  json_t *object;
  char *type = NULL;
  char *iface = NULL;

  object = json_parse_document(LSMessageGetPayload(msg));
  type = object->child->text;

  syslog(LOG_DEBUG, "type %s\n", type);
  if (!type) {
    LS_REPLY_ERROR("No Interface type specified");
    return true;
  }

  if (!strcmp(type, "wifi")) {
    LSCall(priv_serviceHandle, "palm://com.palm.wifi/deleteAP", "{\"ifname\":\"uap0\"}", NULL, NULL, NULL, &lserror);
    LSCall(priv_serviceHandle, "palm://com.palm.wifi/setstate", "{\"state\":\"enabled\"}", NULL, NULL, NULL, &lserror);
  }

  // Only finalize and remove if there are no additional interfaces active
  LSCall(priv_serviceHandle, "palm://com.palm.dhcp/interfaceFinalize", "{\"interface\":\"bridge1\"}", NULL, NULL, NULL, &lserror);
  LSCall(priv_serviceHandle, "palm://com.palm.netroute/removeNetIf", "{\"ifName\":\"bridge1\"}", NULL, NULL, NULL, &lserror);

  LSMessageReply(sh, msg, "{\"returnValue\":true}", &lserror);

  return true;
}

int request_interface(char *type, char *ssid, char *security, char *passphrase) {
  LSError lserror;
  LSErrorInit(&lserror);
  struct interface *iface = NULL;
  struct interface *iter = NULL;
  char *payload = NULL;

  iface = calloc(1, sizeof(*iface));
  if (!iface) {
    syslog(LOG_ERR, "ERROR: Unable to allocate space for additional interface!");
    return -1;
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
  if (type) {
    iface->type = malloc(strlen(type) + 1);
    strcpy(iface->type, type);
  }

  if (ssid) {
    iface->ssid = malloc(strlen(ssid) + 1);
    strcpy(iface->ssid, ssid);
  }

  if (security) {
    iface->security = malloc(strlen(security) + 1);
    strcpy(iface->security, security);
  }

  if (passphrase) {
    iface->passphrase = malloc(strlen(passphrase) + 1);
    strcpy(iface->passphrase, passphrase);
  }
  pthread_mutex_unlock(&iface->mutex);

  asprintf(&payload, "{\"SSID\": \"%s\", \"Security\": \"%s\"", ssid, security);
  if (passphrase /* && not Open */) {
    asprintf(&payload, "%s, \"Passphrase\": %s", payload, passphrase);
  }
  asprintf(&payload, "%s}", payload);

  // Probably have to ifconfig uap0 up here when wifi
  LSCall(priv_serviceHandle, "palm://com.palm.wifi/createAP", payload, create_ap_callback, 
      NULL, NULL, &lserror);

  return 0;
}

bool interfaceAdd(LSHandle *sh, LSMessage *msg, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);
  char *type = NULL;
  char iface[10];
  char command[80];
  json_t *object;
  char *uri = NULL;

  // payload's first label is the type which can be wifi, bluetooth or usb
  object = json_parse_document(LSMessageGetPayload(msg));
  type = object->child->text;
  syslog(LOG_DEBUG, "type %s\n", type);
  if (!type) {
    LS_REPLY_ERROR("No Interface type specified");
    return true;
  }

  if (!strcmp(type, "wifi")) {
    char *ssid = NULL;
    char *security = NULL;
    char *passphrase = NULL;
    char *payload = NULL;
    json_t *wifi = json_find_first_label(object, "wifi");

    json_get_string(wifi->child, "SSID", &ssid);
    json_get_string(wifi->child, "Security", &security);
    json_get_string(wifi->child, "Passphrase", &passphrase);
    syslog(LOG_DEBUG, "ssid %s, security %s, passphrase %s\n", ssid, security, passphrase);

    if (!ssid) {
      LS_REPLY_ERROR("No SSID supplied");
      return true;
    }

    // TODO: Add check for valid security method
    if (!security) {
      security = malloc(strlen("Open") + 1);
      strcpy(security, "Open");
    }

    LSCall(priv_serviceHandle, "palm://com.palm.wifi/setstate", "{\"state\": \"disabled\"}", 
        NULL, NULL, NULL, &lserror);

    // TODO: Add check for passphrase length
    request_interface("WiFi", ssid, security, passphrase);
  }
  else if (!strcmp(type, "bluetooth")) {
    json_get_string(object, "ifname", &iface);
    // Subscribe to com.palm.bluetooth/pan/subscribenotifications
  }
  else if (!strcmp(type, "usb")) {
    json_get_string(object, "ifname", &iface);
  }
  else {
    LS_REPLY_ERROR("Invalid Interface type specified");
    return true;
  }

  if (!iface) {
    LS_REPLY_ERROR("Invalid ifname specified");
    return true;
  }

  // TODO: Use libnetbridge directly
  system("brctl addbr bridge1");
  sprintf(command, "brctl addif bridge1 %s", iface);
  system(command);


  // TODO: callbacks, return values, error checking
  uri = malloc(strlen(DBUS_NETROUTE) + strlen("/addNetIf") + 1);
  sprintf(uri, "%s/addNetIf", DBUS_NETROUTE);
  LSCall(priv_serviceHandle, uri, "{\"ifName\":\"bridge1\", \"networkUsage\":[\"private\"], \"networkTechnology\":\"unknown\", \"networkScope\":\"lan\", \"ipv4\":{\"ip\":\"0x0b01020a\", \"netmask\":\"0x00ffffff\", \"gateway\":\"0x0b02010a\"}}", NULL, NULL, NULL, &lserror);

  uri = realloc(uri, strlen(DBUS_DHCP) + strlen("/interfaceInitialize") + 1);
  sprintf(uri, "%s/interfaceInitialize", DBUS_DHCP);
  LSCall(priv_serviceHandle, uri, "{\"interface\":\"bridge1\", \"mode\":\"server\", \"ipv4Address\":\"10.1.2.11\", \"ipv4Subnet\":\"255.255.255.0\", \"ipv4Router\":\"10.1.2.11\", \"dnsServers\":[\"10.1.2.11\"], \"ipv4RangeStart\":\"10.1.2.70\", \"maxLeases\":15, \"leaseTime\":7200}", NULL, NULL, NULL, &lserror);

  LSMessageReply(sh, msg, "{\"returnValue\":true}", &lserror);

  return true;
}

bool dummy_callback(LSHandle *sh, LSMessage *msg, void *ctx) {
  syslog(LOG_DEBUG, "DUMMY CALLBACK");
  syslog(LOG_DEBUG, "%s", LSMessageGetPayload(msg));
  return true;
}

void start_subscriptions() {
  LSError lserror;
  LSErrorInit(&lserror);

  LS_PRIV_SUBSCRIBE("wifi/interfaceStatus", dummy_callback);
  LS_PRIV_SUBSCRIBE("dhcp/server/leaseInformation", dummy_callback);
}

LSMethod luna_methods[] = {
  {"get_ip_forward", get_ip_forward},
  {"toggle_ip_forward", toggle_ip_forward},
  {"version", version},
  {"interfaceAdd", interfaceAdd},
  {"interfaceRemove", interfaceRemove},
  {"sysInfo", sysInfo},
  {"clientList", clientList},
  {"configRead", configRead},
  {"configWrite", configWrite},
  {"deauthclient", deauthclient},
};

bool register_methods(LSPalmService *serviceHandle, LSError lserror) {
	return LSPalmServiceRegisterCategory(serviceHandle, "/", luna_methods,
			NULL, NULL, NULL, &lserror);
}
