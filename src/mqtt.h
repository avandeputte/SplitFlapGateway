// mqtt.h -- MQTT client, publish queue, and Home Assistant discovery API.

#ifndef SFGW_MQTT_H
#define SFGW_MQTT_H

#include "common.h"
#include "rs485.h"

struct MqttQItem { char topic[48]; char payload[MQTT_BUF_SIZE]; size_t len; };

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

#endif // SFGW_MQTT_H
