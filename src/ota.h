// ota.h -- firmware update: ArduinoOTA task + browser-upload handlers.

#ifndef SFGW_OTA_H
#define SFGW_OTA_H

#include "common.h"

void otaInit();
void taskOTA(void* pv);
void handleOTAPage();
// The name the gateway answers to on the network (cfg.mdnsName, or the default when that is
// empty or unusable), whether a candidate name is a valid one, and how to apply a new one
// without rebooting.
const char* otaMdnsName();
bool        otaMdnsNameValid(const char* name);
void        otaApplyMdnsName();
void wifiSetApActive(bool up);
void handleOTAUpload();
void sendOTAUploadResult();

#endif // SFGW_OTA_H
