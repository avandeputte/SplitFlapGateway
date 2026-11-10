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
    perfLoopTick(PERF_T_RS485);
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

      // A checksummed frame (module firmware v33+) arrives as ~HHHH<frame>\n. Accumulate from
      // the '~' so the prefix can be verified; without this the loop below would skip it and
      // the frame would be parsed as if it had never been checked.
      const bool inCrcPrefix = (lineLen > 0 && lineBuf[0] == '~' && lineLen < CRC_PREFIX_BYTES);

      // If we see an 'm' and the buffer already has content that doesn't start with 'm',
      // discard the stale partial frame and start fresh -- unless what it holds is exactly a
      // checksum prefix, which is what legitimately precedes an 'm'.
      if (c == 'm' && lineLen > 0 && lineBuf[0] != 'm' &&
          !(lineBuf[0] == '~' && lineLen == CRC_PREFIX_BYTES)) {
        lineLen = 0;
      }

      // Start accumulating at the leading 'm', at the '~' of a checksummed frame, or at the
      // '#' of a bootloader frame (a module being updated over the bus speaks only those).
      if (lineLen == 0 && c != 'm' && c != '~' && c != '#') {
        continue;  // skip noise / framing bytes before the message start
      }
      (void)inCrcPrefix;

      // Append byte, guarding against overflow.
      if (lineLen < TX_MAX_BYTES - 1) {
        lineBuf[lineLen++] = c;
      } else {
        // Buffer full without a newline -- corrupt/oversized frame. COUNT it and say so:
        // dropping this silently is what makes "the module never answered" and "the module
        // answered at length and we threw it away" look identical from the outside.
        rxDropped++;
        rxDropBytes = lineLen;
        incidentLog(255, 'D', 0, (uint16_t)lineLen);   // which module it was, nobody can say
        if (traceActive()) {
          traceNote("! discarded %u bytes with no terminator", (unsigned)lineLen);
          // Under msgMutex, like every other traceFrame() call (they all come through
          // ringPush): the formatter has ONE line buffer, and a soak task transmitting on the
          // other core at this moment used to garble exactly the line worth reading.
          if (msgMutex) xSemaphoreTake(msgMutex, portMAX_DELAY);
          traceFrame('!', lineBuf, lineLen, lineLen);    // whole, not cut: these bytes ARE the evidence
          if (msgMutex) xSemaphoreGive(msgMutex);
        }
        printf("[RX] discarded %u bytes with no terminator (frame too long or corrupt)\n",
               (unsigned)lineLen);
        lineLen = 0;
        continue;
      }

      // Newline = end of message.  Commit to ring buffer.
      if (c == '\n') {
        rxCount++;
        /* Verify and strip a checksum prefix before anything else looks at the frame. A frame
           that fails is dropped, not parsed: acting on it is exactly what the checksum exists
           to prevent. The monitor and the trace still see the frame as it arrived. */
        size_t parseOff = 0;
        rxLastChecked = false;
        if (lineLen > CRC_PREFIX_BYTES + 1 && lineBuf[0] == '~') {
          char hex[5] = { (char)lineBuf[1], (char)lineBuf[2], (char)lineBuf[3], (char)lineBuf[4], 0 };
          char* end = NULL;
          const unsigned long want = strtoul(hex, &end, 16);
          const size_t bodyLen = lineLen - CRC_PREFIX_BYTES - 1;      // minus prefix and '\n'
          const uint16_t got = sfCrc16(lineBuf + CRC_PREFIX_BYTES, bodyLen);
          if (end && *end == 0 && (uint16_t)want == got) {
            rxCrcOk++;
            rxLastChecked = true;
            parseOff = CRC_PREFIX_BYTES;
          } else {
            rxCrcFail++;
            int fid = -1;                                   // whose frame it was, if it says so
            if (bodyLen > 1 && lineBuf[CRC_PREFIX_BYTES] == 'm') fid = atoi((const char*)lineBuf + CRC_PREFIX_BYTES + 1);
            printf("[RX] checksum failed on a %u-byte frame (wanted %04lX, got %04X)\n",
                   (unsigned)lineLen, want, got);
            incidentLog(fid >= 0 && fid < 255 ? (uint8_t)fid : 255, 'C', 0, (uint16_t)lineLen);
            if (traceActive()) {
              traceNote("! checksum failed");
              if (msgMutex) xSemaphoreTake(msgMutex, portMAX_DELAY);
              traceFrame('!', lineBuf, lineLen, lineLen);
              if (msgMutex) xSemaphoreGive(msgMutex);
            }
            // still logged to the monitor below, then dropped
          }
        }
        RS485Msg m;
        m.timestamp = millis();
        m.dir       = 'R';
        m.origin    = 0;       // origin only labels 'C' (command) rows, not bus TX/RX
        m.sanitized = false;   // RX frames are never sanitized
        // The monitor ring entry is fixed-size; store at most MSG_MAX_BYTES.
        // The full frame is still parsed below -- this copy is display-only.
        size_t ringLen = (lineLen > RING_FRAME_BYTES) ? RING_FRAME_BYTES : lineLen;
        m.len       = ringLen;
        m.fullLen   = lineLen;            // the monitor shows when a row is truncated
        rxLastLen = lineLen;
        if (lineLen > rxMaxLen) rxMaxLen = lineLen;
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
        // parseOff skips a verified checksum prefix; a frame whose checksum FAILED is not
        // parsed at all, only recorded.
        if (lineBuf[0] == '#') modfwNoteReply(lineBuf, lineLen);        // a bootloader's reply
        else if (!(lineBuf[0] == '~' && !rxLastChecked))
          sfParseResponse(lineBuf + parseOff, lineLen - parseOff);
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
      // Not during a restore-on-boot run: its own read-backs fill the version anyway, and a
      // bare 'v' (answered instantly) sent between a verify query and its reply could collide.
      if (!busBusy() && sfMutex && xSemaphoreTake(sfMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
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
      // Nor during a restore-on-boot run: its verify phase captures ANY dump reply, and a
      // trickle 'A' landing in that window would be read as the wrong module's EEPROM.
      if (!gOtaInProgress && !busBusy() && nowMs - lastFlapQ >= FLAPSET_QUERY_MS) {
        // Stamped HERE, not only when a query goes out. On a wall whose reels are all known
        // nothing is ever sent, so the stamp never moved and this block -- a lock and a walk
        // of the whole registry -- ran on every 5 ms pass of the bus task, for ever.
        lastFlapQ = nowMs;
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
  if (busBusy())  return;        // nor mid-restore: quiet ON would home the wall under it

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

// taskRTC ONLY reads the clock. The quiet-time schedule used to be evaluated here too, and
// that was a crash: taskRTC has a 2 KB stack that already peaks at ~1.4 KB, and asserting
// Quiet Time from it sends bus frames -- sfSetQuietTime -> sfHome -> rs485Send (336 B) ->
// mqttPublishMsg (976 B) plus printf -- about 1.5 KB more. Stack overflow, panic, reboot.
// With restore-on-boot enabled that showed as a loop: boot, restore, quiet asserts, crash,
// boot again. The schedule now ticks on taskNetwork (6 KB). (v3.13.1)
void taskRTC(void* pv) {
  while (true) {
    perfLoopTick(PERF_T_RTC);
    rtcRead();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void taskWeb(void* pv) {
  unsigned long clientSince = 0;   // millis() when current client first seen
  while (true) {
    perfLoopTick(PERF_T_WEB);
    wdgWebMs = millis();      // touch BEFORE handling (covers in-handler stalls)
    server.handleClient();

    // Proactively close any client that lingers connected for too long.
    // The ESP32 WebServer keeps a half-open connection in HC_WAIT_READ for
    // up to HTTP_MAX_DATA_WAIT; a browser (notably Chrome/Safari) that opens
    // a speculative socket and never completes the request can otherwise
    // wedge handleClient() and stall the web task -> "Web=0" watchdog reboot.
    WiFiClient c = server.client();
    if (c && c.connected()) {
      // A client that is connected after handleClient() returns has not sent its request (or
      // has not gone away after its response): the server is held, serving nobody. Counted,
      // because from outside this is indistinguishable from a slow handler.
      perfNoteWait(5);
      if (clientSince == 0) clientSince = millis();
      else if (millis() - clientSince > 8000UL) {   // 8s hard cap per connection
        c.stop();                                    // force-close the stale socket
        clientSince = 0;
      }
    } else {
      clientSince = 0;   // no client connected -- reset the timer
    }

    wdgWebMs = millis();      // touch AFTER handling

    // Self-heal a silently-dead port-80 listener (see webEnsureListening): a ground-truth
    // listening() check every 20s, acted on only when the listener is genuinely down.
    static unsigned long lastListenCheck = 0;
    if (millis() - lastListenCheck >= 20000UL) {
      lastListenCheck = millis();
      webEnsureListening();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool          staWasUp    = false;
unsigned long wifiRetryMs = 0;
static unsigned long ntpTryMs = 0;       // millis() of the last NTP attempt

void taskNetwork(void* pv) {
  // WiFi init done in setup() - this task only polls and reconnects
  while (true) {
    perfLoopTick(PERF_T_NET);
    bool staUp = (WiFi.status() == WL_CONNECTED);
    if (staUp && !staWasUp) {
      staWasUp = true;
      wifiSetApActive(false);   // station is up -> drop the fallback AP
      if (!ntpSynced) { ntpSynced = rtcNTPSync(); ntpTryMs = millis(); }
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
    // NTP used to be tried exactly once per WiFi connection, so a time server that was
    // unreachable at boot (router still starting, DNS not up yet) left the clock unsynced for
    // the whole uptime, and changing the server in Settings set a flag nothing read. Now: at
    // once when asked, and otherwise every 15 minutes until it succeeds -- not more often,
    // because a wall on a network with no route to a time server pays the full timeout on
    // this task for every attempt.
    if (staUp && !gOtaInProgress &&
        (gNtpResyncReq || (!ntpSynced && millis() - ntpTryMs > 900000UL))) {
      gNtpResyncReq = false;
      ntpTryMs  = millis();
      ntpSynced = rtcNTPSync();
    }
    // Requests from other tasks to drop the MQTT session (common.h). Handled here, by the one
    // task that owns the client, and before the OTA check below -- an update starting is the
    // main reason to be asked.
    if (gMqttDropReq || gMqttReconfigReq) {
      const bool reconf = gMqttReconfigReq;
      gMqttDropReq = false; gMqttReconfigReq = false;
      mqtt.disconnect();
      gMqttUp = false;
      if (reconf) {
        // PubSubClient caches the broker it was last told to dial, so a changed setting has
        // to be handed to it again or the reconnect keeps dialling the old one.
        if (strlen(cfg.mqttHost)) mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
        mqttFailCount = 0;                          // a fresh broker gets a fresh strike budget
        mqttRetryMs   = millis() - 30001UL;         // and is dialled now, not in 30 s
      }
    }
    if (staUp && strlen(cfg.mqttHost) && !gOtaInProgress) {
      bool up = mqtt.connected();
      if (!up && millis() - mqttRetryMs > 30000UL) {
        mqttRetryMs = millis();
        mqttConnect();
        up = mqtt.connected();
        if (!up) {
          mqttFailCount++;
          // Five consecutive failures USED to mean "the TCP stack is wedged: drop WiFi and
          // rejoin". It usually means nothing of the kind -- the broker is down, Home
          // Assistant is restarting, the password is wrong -- and the cure then took the
          // whole gateway off the network every two and a half minutes for as long as the
          // broker stayed away: the dashboard, the companion and the REST API all paid for
          // a fault that was not theirs. So the rejoin now needs evidence that it is the
          // stack: the failure was at the transport (never reached a broker at all), AND
          // nobody has got a web request through in five minutes either, AND it was not
          // already tried in the last half hour.
          if (mqttFailCount >= 5) {
            mqttFailCount = 0;
            const int  st        = mqtt.state();
            const bool transport = (st == -2 || st == -4);      // connect failed / timed out
            const bool webIdle   = perfSinceLastRequestMs() > 300000UL;
            static unsigned long lastRejoinMs = 0;
            if (transport && webIdle && (!lastRejoinMs || millis() - lastRejoinMs > 1800000UL)) {
              lastRejoinMs = millis() | 1;
              printf("[MQTT] broker unreachable and no web traffic for 5 min -- rejoining WiFi\n");
              WiFi.disconnect(true);
              delay(500);
              WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
              wifiRetryMs = millis();
            } else {
              printf("[MQTT] still failing (rc=%d) -- will keep retrying every 30 s\n", st);
            }
          }
        } else {
          mqttFailCount = 0;
        }
      }
      gMqttUp = up;
      if (up) {
        mqtt.loop();
        // Drain the outbound queue -- all mqtt.publish calls happen here.
        //
        // One item at a time, copied out UNDER the lock and published AFTER releasing it.
        // publish() is a socket write: on a slow link it blocks for as long as the TCP send
        // buffer stays full, and this loop used to hold mqttQMutex across every one of them.
        // Every producer -- taskRS485, on each frame it receives or sends -- then waited out
        // its 10 ms try-lock and DROPPED the message, so a slow broker both stalled the bus
        // task per frame and lost the frames it was stalling for. Same shape as
        // rs485PollScheduled(): the lock guards the queue, never the I/O.
        if (mqttQMutex && mqttQueue) {
          static MqttQItem out;                       // static: ~1.1 KB, not on this stack
          for (int n = 0; n < MQTT_Q_SIZE; n++) {     // bounded: one queue's worth per pass
            bool have = false;
            if (xSemaphoreTake(mqttQMutex, pdMS_TO_TICKS(10)) != pdTRUE) break;
            if (mqttQTail != mqttQHead) {
              const MqttQItem& item = mqttQueue[mqttQTail];
              strlcpy(out.topic, item.topic, sizeof(out.topic));
              memcpy(out.payload, item.payload, item.len);
              out.len = item.len; out.retain = item.retain;
              mqttQTail = (mqttQTail + 1) % MQTT_Q_SIZE;
              have = true;
            }
            xSemaphoreGive(mqttQMutex);
            if (!have) break;
            // Each publish can block for as long as the socket takes to accept it (10 s if
            // the broker has stopped reading), so this loop feeds the watchdog itself -- three
            // slow ones in a row used to be a "network task stalled" reboot -- and stops at
            // the first failure rather than queueing up behind a link that has gone.
            wdgNetMs = millis();
            if (!mqtt.publish(out.topic, (uint8_t*)out.payload, out.len, out.retain)) break;
          }
        }
      }
    } else {
      gMqttUp = false;
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
    //
    // NOT during an OTA, for the same reason as the registry save below: saveConfig() is a
    // blocking NVS flash write, and stalling the flash while a firmware image streams in starves
    // the TCP receive path. The debounce means this can wait for the post-upload reboot.
    if (gCompanionUrlDirty && !gOtaInProgress &&
        millis() - gCompanionUrlDirtyMs > COMPANION_SAVE_DEBOUNCE_MS) {
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
        // Clear the flag BEFORE the write, not after it. The save is a flash write that takes
        // a real fraction of a second, and it snapshots the registry at its start: a change
        // made by another task while it runs used to be wiped out by the `= false` that
        // followed, and then sat unsaved until something else happened to change.
        sfModulesDirty   = false;
        sfModulesDirtyMs = 0;
        sfModulesSave();
      }
    }
    // Quiet-time schedule, every 5 s (prompt flip at a window boundary). Here and not on
    // taskRTC: asserting quiet sends bus frames, and this task has the stack for that.
    static uint32_t lastSched = 0;
    if (lastSched == 0 || millis() - lastSched > 5000UL) {
      lastSched = millis();
      quietScheduleTick();
    }
    // Drop any firmware hold whose job has stopped, once a second (see bus.h). Cheap, and it
    // is what keeps a cancelled or crashed job from leaving the wall paused.
    static unsigned long lastSweepMs = 0;
    if (millis() - lastSweepMs > 1000UL) { lastSweepMs = millis(); busSweep(); }
    // Periodically prune stale modules (once a minute is plenty).
    static unsigned long lastPruneMs = 0;
    if (!busBusy() && millis() - lastPruneMs > 60000UL) {   // no probes while a restore owns the bus
      lastPruneMs = millis();
      sfModulesPruneStale();
    }

    wdgNetMs = millis();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
