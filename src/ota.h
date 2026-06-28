// ota.h -- firmware update: ArduinoOTA task + browser-upload handlers.

#ifndef SFGW_OTA_H
#define SFGW_OTA_H

#include "common.h"

void otaInit();
void taskOTA(void* pv);
void handleOTAPage();
void wifiSetApActive(bool up);
void handleOTAUpload();
void sendOTAUploadResult();

#endif // SFGW_OTA_H
