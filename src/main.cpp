#include "gateway.h"



// main.cpp -- boot sequence and supervisor.
// setup() brings the system up in dependency order (mutexes and PSRAM buffers,
// config, RTC + module registry, RS-485, WiFi, servers, then the tasks).
// loop() is the watchdog supervisor: it logs periodic telemetry and reboots if
// a task stalls or the heap runs critically low. The protocol/REST/MQTT
// reference lives in the comment block at the bottom of this file.
/* ----------------------------------------------------------
   setup / loop
---------------------------------------------------------- */
void setup() {
  // 1. Mutexes first - must exist before any task touches shared data
  msgMutex   = xSemaphoreCreateMutexStatic(&msgMutexBuf);
  sfMutex    = xSemaphoreCreateMutexStatic(&sfMutexBuf);
  timeMutex  = xSemaphoreCreateMutexStatic(&timeMutexBuf);
  mqttQMutex = xSemaphoreCreateMutexStatic(&mqttQMutexBuf);
  txMutex    = xSemaphoreCreateMutexStatic(&txMutexBuf);
  psramAllocInit();   // allocate large buffers (ring + MQTT queue + registry) in PSRAM

  // Debug output via native USB CDC (USB CDC On Boot: Enabled).
  // Port appears as /dev/cu.usbmodem* on macOS, COMx on Windows, /dev/ttyACM0 on Linux.
  // Connect at 115200 baud.
  Serial.begin(115200);
  { unsigned long t = millis(); while (!Serial && millis() - t < 3000) delay(10); }
  delay(200);
  printf("\n[Boot] Split-Flap Gateway v%s\n", FW_VERSION);
  // Reset reason + chip/heap snapshot -- the first thing to check after an
  // unexpected reboot. PANIC/INT_WDT/TASK_WDT point at firmware faults;
  // BROWNOUT points at power. Pair this with the last [WDG] line before the gap.
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char* rs = "OTHER";
    switch (rr) {
      case ESP_RST_POWERON:  rs = "POWERON";  break;
      case ESP_RST_SW:       rs = "SW";       break;
      case ESP_RST_PANIC:    rs = "PANIC";    break;
      case ESP_RST_INT_WDT:  rs = "INT_WDT";  break;
      case ESP_RST_TASK_WDT: rs = "TASK_WDT"; break;
      case ESP_RST_WDT:      rs = "WDT";      break;
      case ESP_RST_BROWNOUT: rs = "BROWNOUT"; break;
      case ESP_RST_DEEPSLEEP:rs = "DEEPSLEEP";break;
      default: break;
    }
    printf("[Boot] reset=%s heap=%u psram=%u flash=%uKB sdk=%s\n",
           rs, ESP.getFreeHeap(), ESP.getPsramSize(),
           ESP.getFlashChipSize()/1024, ESP.getSdkVersion());
  }

  // 2. Load config and init module registry
  cfgSetDefaults();
  loadConfig();
  memset(sfModules, 0, sizeof(SFModule) * MAX_MODULES);
  for (int i = 0; i < MAX_MODULES; i++) sfModules[i].id = 255;
  sfModuleCount = 0;

  // 3. I2C + RTC (must be before WiFi so timestamps work from boot)
  rtcHwInit();
  rtcRead(); // load whatever time is stored in RTC chip

  // Restore sticky module list from the FATFS file (prunes entries older
  // than 6h). Mount the filesystem first; done after rtcRead() so
  // rtcEpochNow() can evaluate staleness.
  sfFsInit();
  sfModulesLoad();

  // 4. RS485
  rs485Begin();

  // 5. WiFi - MUST be initialised here on the main Arduino task.
  // The SoftAP is a FALLBACK only: start in station mode and connect to the
  // configured network. If no network is configured, bring the fallback AP up
  // immediately so the gateway is reachable for first-time setup. If a network
  // is configured but the station fails to connect, the network task brings the
  // fallback AP up after a grace period (and drops it again once the station
  // connects), so a working WiFi link never leaves the AP running.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  if (strlen(cfg.wifiSSID)) {
    WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
    staDownSince = millis();   // start the fallback grace timer from boot
    printf("[WiFi] STA connecting to %s...\n", cfg.wifiSSID);
  } else {
    wifiSetApActive(true);   // no credentials -> fallback AP for setup
    printf("[WiFi] No network configured -- fallback AP only\n");
  }

  // 6. Web server
  otaInit();
  mqttInit();
  webInit();

  // 7. Spawn tasks after WiFi stack is ready
  xTaskCreatePinnedToCore(taskRTC,     "RTC",     2048, NULL, 2, &hTaskRTC,   0);
  xTaskCreatePinnedToCore(taskRS485,   "RS485",   6144, NULL, 3, &hTaskRS485, 0);
  xTaskCreatePinnedToCore(taskOTA,     "OTA",     4096, NULL, 1, &hTaskOTA,   1);
  xTaskCreatePinnedToCore(taskWeb,     "Web",     8192, NULL, 2, &hTaskWeb,   0);
  xTaskCreatePinnedToCore(taskNetwork, "Network", 6144, NULL, 1, &hTaskNet,   1);

  printf("[Boot] Ready\n");
}

void loop() {
  static unsigned long lastWdgCheck = 0;
  unsigned long now = millis();
  if (now - lastWdgCheck >= 30000UL) {
    lastWdgCheck = now;
    // Rich periodic telemetry for troubleshooting. Heap + min-ever heap +
    // largest free block (fragmentation: a big gap between freeHeap and
    // maxAlloc signals fragmentation, a common pre-crash signature). Per-task
    // stack high-water marks catch the canary-overflow class before it fires.
    // rx/tx/parse-reject counters surface bus health and corruption rates.
    unsigned freeHeap = ESP.getFreeHeap();
    unsigned minHeap  = ESP.getMinFreeHeap();
    unsigned maxBlk   = ESP.getMaxAllocHeap();
    unsigned s485 = hTaskRS485 ? uxTaskGetStackHighWaterMark(hTaskRS485) : 0;
    unsigned sWeb = hTaskWeb   ? uxTaskGetStackHighWaterMark(hTaskWeb)   : 0;
    unsigned sNet = hTaskNet   ? uxTaskGetStackHighWaterMark(hTaskNet)   : 0;
    unsigned sOta = hTaskOTA   ? uxTaskGetStackHighWaterMark(hTaskOTA)   : 0;
    unsigned sRtc = hTaskRTC   ? uxTaskGetStackHighWaterMark(hTaskRTC)   : 0;
    printf("[WDG] up=%lus heap=%u min=%u maxblk=%u frag=%u%% "
           "stk(485/web/net/ota/rtc)=%u/%u/%u/%u/%u "
           "rx=%lu tx=%lu rej=%lu wifi=%d ap=%d rssi=%d mqtt=%d mods=%d\n",
           now/1000, freeHeap, minHeap, maxBlk,
           freeHeap ? (unsigned)(100 - (maxBlk * 100UL / freeHeap)) : 0,
           s485, sWeb, sNet, sOta, sRtc,
           rxCount, txCount, sfParseRejects,
           (int)(WiFi.status()==WL_CONNECTED),
           (int)gApActive,
           (WiFi.status()==WL_CONNECTED) ? (int)WiFi.RSSI() : 0,
           (int)mqtt.connected(), sfModuleCount);

    // Boot grace period: skip stall detection for the first 60s. The first
    // boot after flashing formats the FATFS partition (a long blocking flash
    // operation), and WiFi/MQTT bring-up can briefly skew task scheduling.
    // Rebooting during this window would be a false positive.
    if (now < 60000UL) {
      // still arm the low-heap emergency check below, but skip stall logic
    } else {
      // Detect stalled tasks. A heartbeat in the future (wdg > now) can only
      // come from a transient timing skew during boot -- treat it as healthy
      // rather than letting the unsigned subtraction underflow to a huge value.
      bool ok485 = (wdgRS485Ms == 0 || wdgRS485Ms > now || now - wdgRS485Ms < 30000UL);
      bool okWeb  = (wdgWebMs  == 0 || wdgWebMs  > now || now - wdgWebMs  < 120000UL);
      bool okNet  = (wdgNetMs  == 0 || wdgNetMs  > now || now - wdgNetMs  < 30000UL);
      if (!ok485 || !okWeb || !okNet) {
        printf("[WDG] STALL: RS485=%d Web=%d Net=%d (age485=%lus ageWeb=%lus ageNet=%lus heap=%u) -- rebooting\n",
                      ok485, okWeb, okNet,
                      wdgRS485Ms ? (now - wdgRS485Ms)/1000 : 0,
                      wdgWebMs   ? (now - wdgWebMs)/1000   : 0,
                      wdgNetMs   ? (now - wdgNetMs)/1000   : 0,
                      ESP.getFreeHeap());
        delay(200);
        ESP.restart();
      }
    }
    // Emergency reboot if heap falls critically low (< 20KB)
    if (ESP.getFreeHeap() < 20000) {
      printf("[WDG] CRITICAL: heap=%u -- rebooting\n", ESP.getFreeHeap());
      delay(200);
      ESP.restart();
    }
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
}
