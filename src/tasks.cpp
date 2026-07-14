#include "gateway.h"



// tasks.cpp -- the long-lived FreeRTOS task loops.
// taskRS485 (core 0): drains the UART, frames complete messages, parses replies.
// taskRTC  (core 0): polls the RTC once a second.
// taskWeb  (core 0): services the HTTP server.
// taskNetwork (core 1): WiFi/MQTT reconnect, status publishing, registry
// persistence and stale-module pruning. Each feeds its watchdog timestamp;
// loop() in main.cpp reboots the board if one stalls.
/* ----------------------------------------------------------
   FreeRTOS tasks
---------------------------------------------------------- */

// RS485 receive + response parsing (Core 0)
//
// The split-flap protocol uses newline-terminated ASCII messages that always
// start with 'm'.  We accumulate bytes byte-by-byte into a line buffer and
// only commit a complete message to the ring buffer when we see '\n' (or when
// the buffer is about to overflow).  This guarantees every ring buffer entry
// is exactly one complete protocol message regardless of how the UART delivers
// the bytes (split across reads, multiple messages in one read, etc.).
void taskRS485(void* pv) {
  // Receive accumulator sized for long inbound frames (e.g. a full EEPROM dump
  // response m<id>d:<offset>:<steps>:<map> can reach ~590 bytes). The monitor
  // ring entry (RS485Msg.data) stays at MSG_MAX_BYTES, so the ring copy below
  // is truncated for display while the full frame is parsed.
  static uint8_t lineBuf[TX_MAX_BYTES];
  static size_t  lineLen = 0;

  // Startup discovery: if we booted with an empty registry (first boot, or after
  // an Identify-All / cleared list), broadcast m*v so every module on the bus
  // reports its version + serial and populates the initial list. Delayed briefly
  // so the bus driver and the RX path here are fully up before we transmit.
  {
    vTaskDelay(pdMS_TO_TICKS(1500));
    bool empty;
    if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
    empty = (sfModuleCount == 0);
    if (sfMutex) xSemaphoreGive(sfMutex);
    if (empty) {
      printf("[MOD] empty registry at boot -- broadcasting m*v to discover modules\n");
      rs485SendStr("m*v\n");
    }
  }

  while (true) {
    while (rs485.available()) {
      int b = rs485.read();
      if (b < 0) break;
      uint8_t c = (uint8_t)b;

      // Touch the watchdog inside the byte loop too: a sustained burst of
      // bus traffic (each completed frame triggers rtcFormatTime + MQTT +
      // parse) could otherwise keep us in this inner loop past the 30s
      // RS485 watchdog threshold and trigger a false stall reboot.
      wdgRS485Ms = millis();
      gLastRxMs  = wdgRS485Ms;   // bus activity marker for TX collision avoidance

      // If we see an 'm' and the buffer already has content that doesn't
      // start with 'm', discard the stale partial frame and start fresh.
      if (c == 'm' && lineLen > 0 && lineBuf[0] != 'm') {
        lineLen = 0;
      }

      // Start accumulating only once we've seen the leading 'm'.
      if (lineLen == 0 && c != 'm') {
        continue;  // skip noise / framing bytes before the message start
      }

      // Append byte, guarding against overflow.
      if (lineLen < TX_MAX_BYTES - 1) {
        lineBuf[lineLen++] = c;
      } else {
        // Buffer full without a newline -- corrupt/oversized frame, discard.
        lineLen = 0;
        continue;
      }

      // Newline = end of message.  Commit to ring buffer.
      if (c == '\n') {
        rxCount++;
        RS485Msg m;
        m.timestamp = millis();
        m.dir       = 'R';
        m.origin    = 0;       // origin only labels 'C' (command) rows, not bus TX/RX
        m.sanitized = false;   // RX frames are never sanitized
        // The monitor ring entry is fixed-size; store at most MSG_MAX_BYTES.
        // The full frame is still parsed below -- this copy is display-only.
        size_t ringLen = (lineLen > MSG_MAX_BYTES) ? MSG_MAX_BYTES : lineLen;
        m.len       = ringLen;
        memcpy(m.data, lineBuf, ringLen);
        rtcFormatTime(m.wallTime, sizeof(m.wallTime));
        m.epoch     = rtcEpochNow();  // UTC epoch for browser-local display
        // Log the received frame (strip trailing newline for readability)
        { char dbg[MSG_MAX_BYTES]; size_t dlen = (ringLen > 0) ? ringLen-1 : 0;
          if (dlen > sizeof(dbg) - 1) dlen = sizeof(dbg) - 1;
          memcpy(dbg, lineBuf, dlen); dbg[dlen] = '\0';
          DBG("[RX] %s  (%s)\n", dbg, m.wallTime); }
        ringPush(m);
        mqttPublishMsg(m);
        sfParseResponse(lineBuf, lineLen);   // full frame, not the truncated copy
        lineLen = 0;
      }
    }

    // Deferred post-provision version queries (with retry). When a module's
    // verDueMs arrives we query its version; if no version has come back yet we
    // re-arm for another attempt, up to MODULE_VER_MAX_TRIES. A module is
    // considered done the moment its fwVersion is populated (the version-response
    // handler fills it). Done here (not inline in the ack handler) so the module
    // has settled on its new ID and will actually reply. IDs to query are
    // collected under the lock and sent unlocked (rs485Send re-takes sfMutex via
    // frame tracking -> querying under it would deadlock).
    {
      unsigned long nowMs = millis();
      static uint8_t verDue[MAX_MODULES];
      int verN = 0;
      if (sfMutex && xSemaphoreTake(sfMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < sfModuleCount && verN < MAX_MODULES; i++) {
          SFModule& m = sfModules[i];
          if (!m.verDueMs || nowMs < m.verDueMs) continue;   // not due
          if (m.fwVersion[0]) { m.verDueMs = 0; continue; }  // already known -> stop
          if (m.verTries >= MODULE_VER_MAX_TRIES) {          // gave up
            m.verDueMs = 0;
            // Exhausted the post-provision retries without a version reply. With
            // the newline-collision fixed a direct version query is reliable, so
            // reaching this point is rare (a module that was busy/off-bus the
            // whole window). Just stop the deferred sweep and leave fwVersion
            // empty; the Info dialog or the next Identify will re-query and fill
            // it in. No sentinel is stamped -- mislabeling a healthy module as
            // "older" was a workaround for the (now-fixed) collision bug.
            DBG("[MOD] module %d: no version after %d post-provision tries -- will re-query on demand\n",
                m.id, m.verTries);
            continue;
          }
          verDue[verN++] = m.id;
          m.verTries++;
          m.verDueMs = nowMs + MODULE_VER_RETRY_MS;           // re-arm for a retry
        }
        xSemaphoreGive(sfMutex);
      }
      for (int i = 0; i < verN; i++) {
        DBG("[MOD] post-provision version query -> module %d\n", verDue[i]);
        sfQueryVersion(verDue[i]);
      }
    }

    // The flap-set fill. GET /api/capabilities answers "what can this wall show?", and to do
    // that the gateway needs each module's reel. A module only volunteers it in the v31+ tail
    // of an 'A' reply, and an 'A' reply is ~200 bytes: at 9600 baud, asking 45 modules costs
    // about NINETY SECONDS of bus. So this is a TRICKLE, not a sweep -- ONE module per
    // FLAPSET_QUERY_MS, and only ever a module whose set is not known.
    //
    // On a settled wall it does nothing at all: a set once learned is persisted (see
    // sfModulesLoad), so this runs only after a module is new to the registry, after an 'N'
    // deliberately changed a reel, or after the registry was cleared. Which is the whole reason
    // the set is persisted rather than re-read at boot.
    //
    // A module that will not answer is asked FLAPSET_MAX_TRIES times and then left alone -- it
    // reports as "unknown" in capabilities, which is the truth, rather than being asked forever.
    {
      unsigned long nowMs = millis();
      static unsigned long lastFlapQ = 0;
      // Not while an image is streaming. An 'A' reply is ~200 bytes that get parsed, ring-pushed
      // and MQTT-queued; none of that may compete with the upload for heap. The wall is not going
      // anywhere, and the fill resumes after the reboot.
      if (!gOtaInProgress && nowMs - lastFlapQ >= FLAPSET_QUERY_MS) {
        int askId = -1;
        if (sfMutex && xSemaphoreTake(sfMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          for (int i = 0; i < sfModuleCount; i++) {
            SFModule& m = sfModules[i];
            if (!m.provisioned)                     continue;
            if (m.flapCount > 0 && m.flapChars[0])  continue;   // known
            if (!m.flapDueMs || nowMs < m.flapDueMs) continue;  // not due
            // Older than v31: it cannot answer, and asking is bus time spent to learn nothing.
            // sfFlapSetOf() reports it as "assumed" instead. An UNREAD version (fwVersion "")
            // is not "old" -- leave it due, and the deferred version query will fill it in.
            if (m.fwVersion[0] && atoi(m.fwVersion) > 0 && atoi(m.fwVersion) < FLAPSET_FW_MIN) {
              m.flapDueMs = 0;
              continue;
            }
            if (m.flapTries >= FLAPSET_MAX_TRIES) { m.flapDueMs = 0; continue; }   // gave up
            m.flapTries++;
            m.flapDueMs = nowMs + FLAPSET_RETRY_MS;   // re-arm; a reply clears it (sfParseResponse)
            askId = m.id;
            break;                                    // ONE per interval. It is a trickle.
          }
          xSemaphoreGive(sfMutex);
        }
        if (askId >= 0) {
          lastFlapQ = nowMs;
          DBG("[MOD] flap-set query -> module %d\n", askId);
          char f[16];
          snprintf(f, sizeof(f), "m%dA\n", askId);   // the combined reply; its tail is the set
          rs485SendStr(f);                            // unlocked: rs485Send re-takes sfMutex
        }
      }
    }

    // Send any scheduled batch frames now due -- /api/rs485/batch's cascade pacing,
    // moved off taskWeb so the HTTP server never blocks on delay() (see rs485.h).
    rs485PollScheduled(millis());

    wdgRS485Ms = millis();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// v3.0: evaluate the Quiet-Time schedule and toggle Quiet Time when local time
// crosses a window boundary. Transition-based (only acts on a change), so a
// manual Quiet toggle within a window is respected until the next boundary.
// True if the schedule is enabled and the current user-local time is in-window.
// The schedule (start/end/days) is entered in the browser's local time, and the
// browser sends its UTC offset with it, so we apply that offset and read the
// result as UTC fields -- independent of the gateway posixTZ (which defaults to
// UTC), and consistent with how the web UI renders every other timestamp.
bool quietSchedInWindow() {
  if (!cfg.quietSchedEnabled) return false;
  time_t utc = (time_t)rtcEpochNow();
  if (!utc) return false;            // RTC/NTP not valid yet
  time_t local = utc + (time_t)cfg.quietTzOffsetMin * 60;
  struct tm lt;
  gmtime_r(&local, &lt);             // offset already applied -> read fields directly
  int cur = lt.tm_hour * 60 + lt.tm_min;
  int sh = 22, sm = 0, eh = 7, em = 0;
  sscanf(cfg.quietStart, "%d:%d", &sh, &sm);
  sscanf(cfg.quietEnd,   "%d:%d", &eh, &em);
  int s = sh * 60 + sm, e = eh * 60 + em;
  bool dayOn = (cfg.quietDays >> lt.tm_wday) & 1;
  bool inWin = (s <= e) ? (cur >= s && cur < e) : (cur >= s || cur < e);  // overnight ok
  return dayOn && inWin;
}

static void quietScheduleTick() {
  static int prevWant = -1;
  if (gOtaInProgress) return;        // never touch quiet/resync mid-flash (OTA safety)

  // Does the schedule currently want Quiet ON? A DISABLED schedule counts as
  // want=0 (not a hard early-return) so that disabling an active schedule flows
  // through the same falling-edge turn-off below and RELEASES Quiet Time, rather
  // than leaving it stuck on.
  int want;
  if (!cfg.quietSchedEnabled) {
    want = 0;
  } else {
    if (!rtcEpochNow()) return;      // enabled but no valid time yet -- don't change state
    want = quietSchedInWindow() ? 1 : 0;
  }

  // Self-healing, not edge-only: inside the window keep Quiet ON, re-asserting if
  // anything turned it off (external OFF is also refused mid-window; see the
  // MQTT/REST guards). On the falling edge -- window end OR the schedule being
  // disabled -- turn OFF, but only if the schedule was the one holding it
  // (prevWant==1), so a manual Quiet Time outside the schedule is left alone.
  if (want) {
    if (!gQuietTime) { DBG("[QUIET] schedule: in window, asserting ON\n"); sfSetQuietTime(true); }
  } else if (prevWant == 1 && gQuietTime) {
    DBG("[QUIET] schedule: released (window ended or disabled), turning OFF\n");
    sfSetQuietTime(false);
  }
  prevWant = want;
}

void taskRTC(void* pv) {
  uint32_t lastSched = 0;
  while (true) {
    rtcRead();
    if (lastSched == 0 || millis() - lastSched > 5000UL) {
      lastSched = millis();
      quietScheduleTick();     // evaluate the quiet window every 5s (prompt flip)
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void taskWeb(void* pv) {
  unsigned long clientSince = 0;   // millis() when current client first seen
  while (true) {
    wdgWebMs = millis();      // touch BEFORE handling (covers in-handler stalls)
    server.handleClient();

    // Proactively close any client that lingers connected for too long.
    // The ESP32 WebServer keeps a half-open connection in HC_WAIT_READ for
    // up to HTTP_MAX_DATA_WAIT; a browser (notably Chrome/Safari) that opens
    // a speculative socket and never completes the request can otherwise
    // wedge handleClient() and stall the web task -> "Web=0" watchdog reboot.
    WiFiClient c = server.client();
    if (c && c.connected()) {
      if (clientSince == 0) clientSince = millis();
      else if (millis() - clientSince > 8000UL) {   // 8s hard cap per connection
        c.stop();                                    // force-close the stale socket
        clientSince = 0;
      }
    } else {
      clientSince = 0;   // no client connected -- reset the timer
    }

    wdgWebMs = millis();      // touch AFTER handling
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool          staWasUp    = false;
unsigned long wifiRetryMs = 0;

void taskNetwork(void* pv) {
  // WiFi init done in setup() - this task only polls and reconnects
  while (true) {
    bool staUp = (WiFi.status() == WL_CONNECTED);
    if (staUp && !staWasUp) {
      staWasUp = true;
      wifiSetApActive(false);   // station is up -> drop the fallback AP
      if (!ntpSynced) ntpSynced = rtcNTPSync();
      { IPAddress _a = WiFi.localIP();
  printf("[WiFi] Connected IP=%d.%d.%d.%d\n", _a[0],_a[1],_a[2],_a[3]); }
    } else if (!staUp && staWasUp) {
      staWasUp = false;
      staDownSince = millis();
      printf("[WiFi] Disconnected\n");
    }
    // Fallback AP: if the station has been down for a grace period (and a
    // network is configured), bring the AP up so the gateway stays reachable.
    // If no network is configured the AP was already raised at boot. Skipped
    // during OTA (the AP is intentionally down to free RAM for the upload).
    if (!staUp && !gApActive && !gOtaInProgress && strlen(cfg.wifiSSID) &&
        staDownSince && millis() - staDownSince > 20000UL) {
      printf("[WiFi] Station down 20s -- raising fallback AP\n");
      wifiSetApActive(true);
    }
    if (!staUp && strlen(cfg.wifiSSID) && millis() - wifiRetryMs > 15000UL) {
      wifiRetryMs = millis();
      WiFi.disconnect();
      WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
    }
    if (staUp && strlen(cfg.mqttHost) && !gOtaInProgress) {
      if (!mqtt.connected() && millis() - mqttRetryMs > 30000UL) {
        mqttRetryMs = millis();
        mqttConnect();
        if (!mqtt.connected()) {
          mqttFailCount++;
          // After 5 consecutive failures with WiFi "up", the TCP stack
          // is likely wedged. Force a full WiFi reconnect to recover.
          if (mqttFailCount >= 5) {
            printf("[MQTT] %d consecutive failures -- forcing WiFi reconnect\n",
                   mqttFailCount);
            mqttFailCount = 0;
            WiFi.disconnect(true);
            delay(500);
            WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
            wifiRetryMs = millis();
          }
        } else {
          mqttFailCount = 0;
        }
      }
      if (mqtt.connected()) {
        mqtt.loop();
        // Drain the outbound queue -- all mqtt.publish calls happen here
        if (mqttQMutex && mqttQueue && xSemaphoreTake(mqttQMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          while (mqttQTail != mqttQHead) {
            MqttQItem& item = mqttQueue[mqttQTail];
            mqtt.publish(item.topic, (uint8_t*)item.payload, item.len, false);
            mqttQTail = (mqttQTail + 1) % MQTT_Q_SIZE;
          }
          xSemaphoreGive(mqttQMutex);
        }
      }
    }
    if (!gOtaInProgress && millis() - lastStatusMs > STATUS_INTERVAL_MS) {
      lastStatusMs = millis();
      mqttPublishStatus();
    }
    // Refresh the HA display sensor when tracking changed, rate-limited to avoid
    // spamming HA's recorder. No-op unless HA integration is enabled. Skipped
    // during an OTA upload to keep heap/CPU free for the transfer.
    if (!gOtaInProgress && gDisplayDirty && cfg.haEnabled && millis() - lastDispPubMs > 1500) {
      gDisplayDirty = false;
      lastDispPubMs = millis();
      mqttPublishDisplayState();
    }

    // Persist the module registry if it changed (debounced to limit NVS wear).
    // Persist the companion URL only once it has stopped moving. See the note on
    // gCompanionUrlDirty in common.h: an unconditional save turned two co-resident
    // companions into an NVS write every heartbeat, forever.
    if (gCompanionUrlDirty && millis() - gCompanionUrlDirtyMs > COMPANION_SAVE_DEBOUNCE_MS) {
      gCompanionUrlDirty = false;
      saveConfig();
      DBG("[CFG] companion URL settled -- persisted: %s\n", cfg.companionUrl);
    }
    // NOT during an OTA. sfModulesSave() writes FATFS -- the same SPI flash the image is being
    // written to -- and a registry save that lands mid-upload stalls the flash long enough to
    // starve the TCP receive path. The save is debounced anyway; it can wait for the reboot.
    if (sfModulesDirty && !gOtaInProgress) {
      if (sfModulesDirtyMs == 0) sfModulesDirtyMs = millis();
      if (millis() - sfModulesDirtyMs > MODULE_SAVE_DEBOUNCE_MS) {
        sfModulesSave();
        sfModulesDirty   = false;
        sfModulesDirtyMs = 0;
      }
    }
    // Periodically prune stale modules (once a minute is plenty).
    static unsigned long lastPruneMs = 0;
    if (millis() - lastPruneMs > 60000UL) {
      lastPruneMs = millis();
      sfModulesPruneStale();
    }

    wdgNetMs = millis();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
