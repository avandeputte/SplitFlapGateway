// mqtt.h -- MQTT client, publish queue, and Home Assistant discovery API.

#ifndef SFGW_MQTT_H
#define SFGW_MQTT_H

#include "common.h"
#include "rs485.h"

// `topic` was 48 bytes, and a Home Assistant discovery topic is longer than that:
//   homeassistant/switch/sfgw_1234ABCD/maintenance/config   is 53 characters,
//   homeassistant/sensor/sfgw_1234ABCD/minheap/config       is 49.
// strlcpy() cut them to 47, so the Maintenance switch and eleven of the eighteen diagnostic
// sensors were announced on topics ending in "/conf" or "/" -- which Home Assistant ignores --
// and never appeared. The queue lives in PSRAM, so the room costs nothing that matters.
struct MqttQItem { char topic[96]; char payload[MQTT_BUF_SIZE]; size_t len; bool retain; };

// ---- owned globals (defined in globals.cpp) ----
extern int mqttFailCount;
extern MqttQItem* mqttQueue;
extern volatile int mqttQHead;
extern volatile int mqttQTail;
extern SemaphoreHandle_t mqttQMutex;
extern StaticSemaphore_t mqttQMutexBuf;
extern WiFiClient wifiClient;
extern WiFiClient mqttWifiClient;
extern PubSubClient mqtt;
extern unsigned long lastStatusMs;
extern unsigned long lastDispPubMs;
extern unsigned long mqttRetryMs;

void mqttPublishMsg(const RS485Msg& m);
void mqttPublishSFEvent(const char* event, const char* payload);
void mqttPublishStatus();
void mqttPublishDisplayState();
void mqttPublishStateTopics();
void haPublishDiscovery(bool enable);
void mqttInit();
void mqttConnect();
const char* mqttStateName(int rc);

// ---- connection history (v4.5) ----
// "MQTT keeps dropping" used to leave nothing behind: the reason was printed on a serial
// port nobody had attached, and the dashboard showed a red dot. So every change in the link
// -- WiFi up and down, a broker session made, lost or refused, and every time somebody saved
// the MQTT settings (and what that did to the stored password) -- is kept in a small ring,
// with the time, the reason code, the signal strength and how long the thing had lasted.
//
// The ring lives in RTC memory: it survives a restart, including a crash or watchdog restart
// -- which is when it is wanted most -- and is empty only after a power cut. GET /api/mqtt/log.
enum : uint8_t {
  CEV_BOOT = 1,      // the gateway started. rc = esp_reset_reason()
  CEV_WIFI_UP,       // joined the network. rc = channel, arg = seconds it had been down
  CEV_WIFI_DOWN,     // left it. rc = the WiFi stack's disconnect reason, arg = seconds it had been up
  CEV_MQTT_UP,       // broker session established
  CEV_MQTT_LOST,     // an established session ended without being asked to. rc = PubSubClient
                     // state (-3 closed, -4 keep-alive timed out) or CEV_RC_WIFI; arg = seconds it lasted
  CEV_MQTT_FAILED,   // a connect attempt failed. rc = PubSubClient state. Repeats fold into one row (n)
  CEV_MQTT_CLOSED,   // the gateway closed the session itself. rc = CEV_RC_UPDATE / CEV_RC_SETTINGS
  CEV_SETTINGS,      // MQTT settings were saved. flags = CSET_*, arg = the client's IPv4 address
  CEV_KIND_MAX
};
#define CEV_RC_WIFI      (-10)   // CEV_MQTT_LOST: WiFi went down under it (flags = the WiFi reason)
#define CEV_RC_UPDATE    0       // CEV_MQTT_CLOSED: a firmware update is starting
#define CEV_RC_SETTINGS  1       // CEV_MQTT_CLOSED: to apply saved settings
#define CSET_HOST         0x01   // broker host or port changed
#define CSET_USER         0x02   // username changed
#define CSET_PASS_CHANGED 0x04   // a new password was stored
#define CSET_PASS_CLEARED 0x08   // the stored password was removed
#define CSET_PASS_KEPT    0x10   // the password field was blank and the stored one was kept
#define CSET_PREFIX       0x20   // topic prefix changed

struct ConnEvent {
  uint32_t epoch;    // wall clock at the (latest) occurrence; 0 if the clock was not set yet
  uint32_t upS;      // uptime, seconds, at the (latest) occurrence
  uint32_t arg;      // see the kinds above
  uint16_t n;        // occurrences folded into this row (CEV_MQTT_FAILED repeats)
  int16_t  rc;
  int8_t   rssi;     // dBm when it happened, 0 if not associated
  uint8_t  kind;
  uint8_t  boot;     // which boot it belongs to (a counter, mod 256)
  uint8_t  flags;
};
#define CONN_LOG_SIZE 24

void connLogInit();                                   // once, from setup(), before the tasks start
void connLogAdd(uint8_t kind, int rc, uint32_t arg = 0, uint8_t flags = 0);
// Copies the newest `max` rows, newest first. Returns how many. `lastDrop` (may be NULL) gets
// the most recent CEV_MQTT_LOST, kept apart so a flood of retries cannot push it out;
// its kind is 0 if there has been none.
int  connLogCopy(ConnEvent* out, int max, ConnEvent* lastDrop, uint8_t* bootNow);
const char* connEventKind(uint8_t kind);              // "mqtt-lost", "wifi-down", ...
void connEventWhy(const ConnEvent& e, char* buf, size_t len);   // the reason, in English
extern volatile uint32_t gMqttConnects, gMqttDrops, gMqttFails;   // since this boot
extern volatile unsigned long gMqttSinceMs;           // when the session came up, or went down
extern volatile uint8_t gWifiDropReason;              // first disconnect reason since the last join

#endif // SFGW_MQTT_H
