#include "gateway.h"



// ota.cpp -- firmware over-the-air update.
// Two paths to the same single binary: ArduinoOTA (IDE push, driven by taskOTA)
// and a browser upload (handleOTAUpload via the Update library). During an
// upload gOtaInProgress throttles other work and the AP/modem-sleep are tuned
// to free heap for the transfer; otaRestoreWifi puts things back afterwards.
// ---- file-private forward declarations ----
static void otaRestoreWifi();

// ---------------------------------------------------------------------------
// OTA update support
// ---------------------------------------------------------------------------
void otaInit() {
  // The hostname carries the board id: two gateways on one LAN both called
  // "splitflap-gw" fight over the mDNS name exactly the way they fought over
  // the MQTT client id (see boardId24() in common.h — this is the same fix).
  // splitflap-gw-<6 hex digits>, unique per board, printed at boot below.
  static char hostname[24];
  snprintf(hostname, sizeof(hostname), "splitflap-gw-%06x", (unsigned)boardId24());
  ArduinoOTA.setHostname(hostname);

  // Optional password protection
  if (strlen(cfg.otaPassword) > 0) {
    ArduinoOTA.setPassword(cfg.otaPassword);
  }

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    printf("[OTA] Starting %s update\n", type.c_str());
  });
  ArduinoOTA.onEnd([]() {
    printf("[OTA] Update complete -- rebooting\n");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static uint8_t lastPct = 255;
    uint8_t pct = (uint8_t)(progress * 100 / total);
    if (pct != lastPct && pct % 10 == 0) {
      printf("[OTA] Progress: %u%%\n", pct);
      lastPct = pct;
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    const char* msg = "Unknown";
    if      (error == OTA_AUTH_ERROR)    msg = "Auth failed";
    else if (error == OTA_BEGIN_ERROR)   msg = "Begin failed";
    else if (error == OTA_CONNECT_ERROR) msg = "Connect failed";
    else if (error == OTA_RECEIVE_ERROR) msg = "Receive failed";
    else if (error == OTA_END_ERROR)     msg = "End failed";
    printf("[OTA] Error: %s\n", msg);
  });

  ArduinoOTA.begin();
  // ArduinoOTA.begin() started the mDNS responder with our hostname; also
  // advertise the web UI so browsers can reach http://<hostname>.local
  MDNS.addService("http", "tcp", 80);
  printf("[OTA] Ready (hostname: %s, web UI at http://%s.local)\n", hostname, hostname);
}

// OTA runs in its own task so ArduinoOTA.handle() is called frequently
// without blocking the web server or RS485 tasks.
void taskOTA(void* pv) {
  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      ArduinoOTA.handle();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}


// ---------------------------------------------------------------------------
// Web-based OTA firmware upload
// ---------------------------------------------------------------------------
void handleOTAPage() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // Served as a standalone page at /ota so the upload iframe works cleanly
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Firmware Update</title>"
    "<link rel='icon' type='image/svg+xml' href='/favicon.svg'>"
    "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eaeaea;padding:30px}"
    "h2{color:#e94560}progress{width:100%;height:20px;margin-top:10px}"
    "input[type=file]{color:#eaeaea}button{margin-top:10px;padding:8px 20px;"
    "background:#e94560;border:none;color:#fff;border-radius:4px;cursor:pointer}"
    "#status{margin-top:14px;font-size:.9rem}</style></head><body>"
    "<h2>Firmware Update</h2>"
    "<p style='color:#888;font-size:.85rem'>Select a compiled .bin file. "
    "The gateway verifies the image when the last byte lands, then reboots into it. "
    "The progress bar shows the file leaving your browser; the gateway writes it to "
    "flash at about 90 KB/s, so a full image takes 15-20 s after the bar fills.</p>"
    "<input type='file' id='fw' accept='.bin'>"
    "<br><button onclick='upload()'>Upload Firmware</button>"
    "<progress id='prog' value='0' max='100' style='display:none'></progress>"
    "<div id='status'></div>"
    "<script>"
    "function upload(){"
    "var f=document.getElementById('fw').files[0];"
    "if(!f){document.getElementById('status').textContent='No file selected.';return;}"
    "var fd=new FormData();fd.append('firmware',f,f.name);"
    "var xhr=new XMLHttpRequest();"
    "xhr.upload.onprogress=function(e){"
    "if(e.lengthComputable){"
    "var p=Math.round(e.loaded*100/e.total);"
    "document.getElementById('prog').style.display='';"
    "document.getElementById('prog').value=p;"
    "document.getElementById('status').textContent='Uploading: '+p+'%';}"
    "};"
    "var t0=Date.now();"
    "function back(){"
    "fetch('/api/config',{cache:'no-store'}).then(function(r){return r.json();}).then(function(c){"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(76,175,80)\">Rebooted. The gateway is back on firmware v'+(c.version||'?')+'. <a href=\"/\" style=\"color:#7c5cff\">Open the dashboard</a></span>';"
    "}).catch(function(){"
    "if(Date.now()-t0<90000)setTimeout(back,2000);"
    "else document.getElementById('status').innerHTML='<span style=\"color:rgb(233,69,96)\">The gateway has not come back after 90 s. Check its power and WiFi, then reload.</span>';"
    "});}"
    "xhr.onload=function(){"
    "if(xhr.status===200){"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(76,175,80)\">Image verified -- rebooting into it now. Waiting for the gateway to come back...</span>';"
    "setTimeout(back,4000);"
    "}else{"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(233,69,96)\">Error: '+(xhr.responseText||'upload failed')+' The gateway is still running its previous firmware.</span>';}"
    "};"
    "xhr.onerror=function(){"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(233,69,96)\">The connection dropped during the upload. Nothing was flashed; the gateway is still running its previous firmware. "
    "Reload this page and try again, or use <code>pio run -e esp32s3_ota -t upload</code>.</span>';"
    "};"
    "xhr.open('POST','/api/ota/upload');"
    "xhr.send(fd);"
    "}"
    "</script></body></html>";
  server.send(200, "text/html", html);
}

// Bring the fallback SoftAP up or down, switching WiFi mode accordingly.
//   AP up   -> WIFI_AP_STA (AP for config + station keeps trying/holding link)
//   AP down -> WIFI_STA    (station only; no AP buffers/beacons)
// Only acts on an actual change so we never thrash the radio. The AP is a
// fallback: callers bring it up when the station is down (or no credentials are
// configured) and drop it once the station connects.
void wifiSetApActive(bool up) {
  if (up == gApActive) return;
  if (up) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS);
    IPAddress b = WiFi.softAPIP();
    printf("[WiFi] Fallback AP up: %s  %d.%d.%d.%d\n", DEFAULT_AP_SSID, b[0],b[1],b[2],b[3]);
  } else {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    printf("[WiFi] Fallback AP down -- station-only\n");
  }
  gApActive = up;
}

// Tracks whether the in-progress OTA upload has hit a fatal error, so we can
// reject cleanly at the end instead of rebooting into a half-written image.
bool otaUploadFailed = false;

// Restore normal WiFi after a failed/aborted OTA. During an upload we force
// modem sleep off and (if it was up) the AP down to free RAM; afterwards we
// reconcile to the correct state: AP stays down while the station is connected,
// and comes back as a fallback only if the station is not connected.
static void otaRestoreWifi() {
  WiFi.setSleep(true);
  // AP is a fallback: bring it back only if the station is not connected.
  bool staUp = (WiFi.status() == WL_CONNECTED);
  wifiSetApActive(!staUp);
}

void handleOTAUpload() {
  // NO sendHeader() here. This callback runs once per received chunk -- ~1000 times for a
  // 1.5 MB image -- and WebServer::sendHeader() APPENDS its line to a String that is only
  // cleared when the response goes out. A header added per chunk therefore grew a ~30 KB
  // String through ~1000 reallocations in a ~100 KB heap, and the upload died of heap
  // exhaustion near the end of every full-size image (seen at 1.17-1.30 MB, with min-free
  // heap under 1 KB) regardless of how fast the client sent it. The CORS header belongs on
  // the response, and sendOTAUploadResult() sets it there, once.

  // The firmware binary arrives as a multipart/form-data file part. The ESP32
  // WebServer streams it to us in chunks via server.upload(); the empty POST
  // body handler registered alongside this callback sends the final response.
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaUploadFailed = false;
    printf("[OTA] Web upload start: %s\n", upload.filename.c_str());
    // Quiesce the gateway for the duration of the upload: stop MQTT publishing
    // and free its buffers so the WiFi/TCP stack has the contiguous heap the
    // upload needs. gOtaInProgress also makes the network task skip its periodic
    // status/display/discovery publishes. This addresses mid-upload connection
    // drops seen under heap fragmentation (esp. with Home Assistant enabled).
    gOtaInProgress = true;
    if (mqtt.connected()) { mqtt.disconnect(); printf("[OTA] MQTT paused during upload\n"); }
    // Free internal RAM for the transfer. A large firmware streams in faster than
    // flash can absorb it, so WiFi/lwIP receive buffers pile up; on this already
    // heap-constrained, fragmented gateway that can exhaust the heap mid-upload
    // (observed min-free-heap dropping to ~512 bytes -> connection reset). Two
    // levers help: (a) drop the SoftAP so its interface buffers/housekeeping are
    // released (only safe when the station is connected, else we'd lose access),
    // and (b) disable modem sleep so the station drains the RX queue at full
    // speed, reducing buffer buildup. Both are restored if the upload fails.
    if (WiFi.status() == WL_CONNECTED) {
      wifiSetApActive(false);   // drop fallback AP if it happens to be up
      WiFi.setSleep(false);
      printf("[OTA] AP down + modem sleep off for upload (heap=%u)\n", ESP.getFreeHeap());
    }
    // UPDATE_SIZE_UNKNOWN lets the Update library size the partition itself.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaUploadFailed = true;
      gOtaInProgress = false;
      otaRestoreWifi();
      printf("[OTA] Begin failed (%s) -- aborting upload\n", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // handleClient() does not return during a multipart upload, so the web task
    // can't touch its watchdog from its loop -- feed it here on every chunk so a
    // large/slow upload can't trip the 30s web-stall reboot.
    wdgWebMs = millis();
    // Skip writing once we've failed, so we don't keep feeding a dead Update.
    if (!otaUploadFailed) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        otaUploadFailed = true;
        printf("[OTA] Write error (%s) -- aborting upload\n", Update.errorString());
        Update.abort();
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaUploadFailed) {
      Update.abort();
      gOtaInProgress = false;
      otaRestoreWifi();
      printf("[OTA] Upload ended in failed state -- image discarded\n");
      // Response is sent by the POST body handler (sendOTAUploadResult).
    } else if (Update.end(true)) {   // true = set the new image as bootable
      printf("[OTA] Web upload complete (%u bytes) -- verified, rebooting\n",
             upload.totalSize);
      // gOtaInProgress stays set: we reboot momentarily; no need to resume.
    } else {
      otaUploadFailed = true;
      gOtaInProgress = false;
      otaRestoreWifi();
      printf("[OTA] Update.end failed (%s) -- incomplete or corrupt image\n",
             Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    otaUploadFailed = true;
    gOtaInProgress = false;
    Update.abort();
    otaRestoreWifi();
    printf("[OTA] Upload aborted by client -- image discarded\n");
  }
}

// Final response for the OTA upload POST. Runs after the whole multipart body
// (and thus all handleOTAUpload chunk callbacks) has been processed, so by now
// otaUploadFailed reflects the true outcome. On success we reply 200 then
// reboot into the freshly flashed image; on failure we reply 500 and stay up.
void sendOTAUploadResult() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (otaUploadFailed || !Update.isFinished()) {
    server.send(500, "text/plain",
                "Update failed -- firmware not flashed. Device left unchanged.");
    return;
  }
  server.send(200, "text/plain", "OK");
  delay(500);        // let the response flush to the browser before we restart
  ESP.restart();
}
