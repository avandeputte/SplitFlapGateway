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
// A usable mDNS label: lowercase letters, digits and hyphens, starting and ending on a
// letter or digit, 1-31 characters. That is a DNS label's rule, and it is also what a person
// can type into a browser without surprises. Upper case is folded by the caller, not here.
bool otaMdnsNameValid(const char* name) {
  size_t n = name ? strlen(name) : 0;
  if (n < 1 || n > MDNS_NAME_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum && !(c == '-' && i > 0 && i < n - 1)) return false;
  }
  return true;
}

// The name this gateway answers to, as configured -- or the default if the stored one is
// missing or unusable, so a bad value can never leave the board unreachable by name.
const char* otaMdnsName() {
  return otaMdnsNameValid(cfg.mdnsName) ? cfg.mdnsName : DEFAULT_MDNS_NAME;
}

// Start (or restart) the responder under the configured name.
//
// The responder is driven directly rather than through ArduinoOTA.setHostname(), which only
// takes effect the FIRST time: it is guarded by `if (!_initialized && !_hostname.length())`, so
// after boot it silently does nothing and a rename would re-announce the old name (it did --
// the new name simply never appeared on the wire). So ArduinoOTA keeps its UDP listener and we
// keep the mDNS registration: MDNS.begin() sets the host, enableArduino() publishes the
// _arduino._tcp record espota and the Arduino IDE discover, and _http._tcp is the web UI.
static void otaStartMdns() {
  const char* name = otaMdnsName();
  MDNS.begin(name);
  MDNS.enableArduino(3232, strlen(cfg.otaPassword) > 0);
  MDNS.addService("http", "tcp", 80);
  printf("[OTA] mDNS name: %s (http://%s.local)\n", name, name);
}

// Apply a new name without a reboot. Anything already open keeps working by IP; the new name is
// announced at once, and the old one disappears from the network as its records expire.
void otaApplyMdnsName() {
  MDNS.end();
  otaStartMdns();
}

void otaInit() {
  // The name is a setting (default "splitflap", so the wall is at http://splitflap.local).
  // Two gateways on one LAN must not share it: mDNS has no way to tell them apart, exactly as
  // they would fight over an MQTT client id, so the second one gets renamed in Settings. The
  // board id is still what makes the MQTT client id unique -- see boardId24() in common.h.

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

  // mDNS is ours to manage (see otaStartMdns), so ArduinoOTA only listens on its UDP port.
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.setHostname(otaMdnsName());        // for its own logging; mDNS is set below
  if (strlen(cfg.otaPassword) > 0) ArduinoOTA.setPassword(cfg.otaPassword);
  ArduinoOTA.begin();
  otaStartMdns();
  printf("[OTA] Ready (web UI at http://%s.local)\n", otaMdnsName());
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
// Web-based gateway firmware upload
// ---------------------------------------------------------------------------
void handleOTAPage() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // Served as a standalone page at /ota so the upload iframe works cleanly
  // The page polls for the gateway coming back after the reboot. If it was opened by mDNS
  // name (splitflap-gw-xxxxxx.local), that poll depends on the name still resolving the moment
  // the board returns -- and a resolver that cached the old record, a network that blocks
  // multicast, or a phone that never did .local at all turns a SUCCESSFUL flash into "the
  // gateway has not come back". So the page carries the station's own IP address and falls
  // back to it. Same-origin first (it is usually right), the literal address second.
  IPAddress ip = WiFi.localIP();
  char ipStr[20];
  snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Gateway Firmware Update</title>"
    "<link rel='icon' type='image/svg+xml' href='/favicon.svg'>"
    "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eaeaea;padding:30px}"
    "h2{color:#e94560}progress{width:100%;height:20px;margin-top:10px}"
    "input[type=file]{color:#eaeaea}button{margin-top:10px;padding:8px 20px;"
    "background:#e94560;border:none;color:#fff;border-radius:4px;cursor:pointer}"
    "#status{margin-top:14px;font-size:.9rem}</style></head><body>"
    "<h2>Gateway Firmware Update</h2>"
    "<p style='color:#888;font-size:.85rem'>This updates <b>the gateway itself</b>, not the modules. "
    "Choose <b>SplitFlapGateway-&lt;version&gt;.bin</b> from the release &mdash; the plain one. "
    "The <b>.factory.bin</b> is a full-flash image for a blank board over USB and is refused here. "
    "The gateway verifies the image when the last byte lands, then reboots into it. "
    "The progress bar shows the file leaving your browser; the gateway writes it to "
    "flash at about 90 KB/s, so a full image takes 15-20 s after the bar fills.</p>"
    "<input type='file' id='fw' accept='.bin'>"
    "<br><button onclick='upload()'>Upload Gateway Firmware</button>"
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
    "var t0=Date.now();var GWIP='" + String(ipStr) + "';"
    "function back(){"
    // Try this page's own origin first, then the gateway's address. Whichever answers wins,
    // and the link offered afterwards points at the one that worked.
    "var urls=[location.origin,'http://'+GWIP];"
    "var i=0;"
    "function tryOne(){"
    "if(i>=urls.length){"
    "if(Date.now()-t0<90000)setTimeout(function(){i=0;tryOne();},2000);"
    "else document.getElementById('status').innerHTML='<span style=\"color:rgb(233,69,96)\">The gateway has not come back after 90 s. Check its power and WiFi, then reload. It may also be back at <a href=\"http://'+GWIP+'/\" style=\"color:#7c5cff\">'+GWIP+'</a> under a name your computer cannot look up.</span>';"
    "return;}"
    "var base=urls[i];"
    "fetch(base+'/api/config',{cache:'no-store'}).then(function(r){return r.json();}).then(function(c){"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(76,175,80)\">Rebooted. The gateway is back on firmware v'+(c.version||'?')+'. <a href=\"'+base+'/\" style=\"color:#7c5cff\">Open the dashboard</a></span>';"
    "}).catch(function(){i++;tryOne();});}"
    "tryOne();}"
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
// Why it failed, in words the person at the browser can act on. Empty until something goes
// wrong; sendOTAUploadResult() puts it in the 500 body, and the updater page shows that text.
static char otaFailMsg[192] = "";
// Whether the first chunk's header has been checked yet (see the app-descriptor test below).
static bool otaHeaderSeen = false;

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
    otaFailMsg[0] = 0;
    otaHeaderSeen  = false;
    printf("[OTA] Web upload start: %s (heap=%u)\n", upload.filename.c_str(), ESP.getFreeHeap());
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

    // Check the FIRST chunk before streaming 1.5 MB that cannot possibly work. An ESP32
    // application image carries the app descriptor's magic word at offset 0x20; a full-flash
    // image (SplitFlapGateway-<v>.factory.bin) has the bootloader there instead, and its app
    // only starts at 0x10000. Picking the factory file is the common mistake -- it used to
    // upload for eight seconds and end in a bare "Update failed", which is indistinguishable
    // from a real fault and is most of what "the updater is broken" turns out to be.
    if (!otaHeaderSeen && !otaUploadFailed) {
      otaHeaderSeen = true;
      if (upload.currentSize >= 0x24) {
        uint32_t descMagic;
        memcpy(&descMagic, upload.buf + 0x20, sizeof(descMagic));
        if (upload.buf[0] != 0xE9) {
          otaUploadFailed = true;
          strlcpy(otaFailMsg, "That is not an ESP32 firmware image. Upload the plain "
                              "SplitFlapGateway-<version>.bin.", sizeof(otaFailMsg));
        } else if (descMagic != 0xABCD5432UL) {
          otaUploadFailed = true;
          strlcpy(otaFailMsg, "That looks like a full-flash image (.factory.bin), which can "
                              "only be written over USB. Upload the plain "
                              "SplitFlapGateway-<version>.bin instead.", sizeof(otaFailMsg));
        }
        if (otaUploadFailed) {
          printf("[OTA] Refused: %s\n", otaFailMsg);
          Update.abort();
        }
      }
    }

    // Skip writing once we've failed, so we don't keep feeding a dead Update.
    if (!otaUploadFailed) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        otaUploadFailed = true;
        snprintf(otaFailMsg, sizeof(otaFailMsg), "Flash write failed: %s", Update.errorString());
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
      snprintf(otaFailMsg, sizeof(otaFailMsg), "The image did not verify: %s",
               Update.errorString());
      gOtaInProgress = false;
      otaRestoreWifi();
      printf("[OTA] Update.end failed (%s) -- incomplete or corrupt image\n",
             Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    otaUploadFailed = true;
    strlcpy(otaFailMsg, "The upload stopped before the whole image had arrived.",
            sizeof(otaFailMsg));
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
  printf("[OTA] Upload finished -- heap %u, low-water %u\n",
         ESP.getFreeHeap(), ESP.getMinFreeHeap());
  if (otaUploadFailed || !Update.isFinished()) {
    char body[220];
    snprintf(body, sizeof(body), "%s The gateway is unchanged and still running its "
                                 "previous firmware.",
             otaFailMsg[0] ? otaFailMsg : "The update failed before the image was complete.");
    server.send(500, "text/plain", body);
    return;
  }
  server.send(200, "text/plain", "OK");
  delay(500);        // let the response flush to the browser before we restart
  ESP.restart();
}
