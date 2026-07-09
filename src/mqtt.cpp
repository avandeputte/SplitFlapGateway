#include "gateway.h"



// mqtt.cpp -- MQTT integration.
// Any task enqueues with mqttEnqueue/mqttPublish*; only taskNetwork actually
// touches the PubSubClient (connect, loop, and draining the outbound queue) so
// the library is single-threaded. Also publishes Home Assistant discovery and
// entity state, and handles inbound command topics in mqttCallback.
// ---- file-private forward declarations ----
static void mqttCallback(char* topic, byte* payload, unsigned int length);
static void mqttEnqueue(const char* topic, const char* payload, size_t len);

/* ----------------------------------------------------------
   MQTT
---------------------------------------------------------- */
// mqttTopic() removed -- all call sites use snprintf char arrays
// Safe MQTT publish from any task -- enqueues for the network task to drain.
static void mqttEnqueue(const char* topic, const char* payload, size_t len) {
  if (!mqttQMutex || !mqttQueue) return;
  if (xSemaphoreTake(mqttQMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  int next = (mqttQHead + 1) % MQTT_Q_SIZE;
  if (next != mqttQTail) {
    strlcpy(mqttQueue[mqttQHead].topic, topic, sizeof(mqttQueue[0].topic));
    size_t copy = (len < sizeof(mqttQueue[0].payload)-1) ? len : sizeof(mqttQueue[0].payload)-1;
    memcpy(mqttQueue[mqttQHead].payload, payload, copy);
    mqttQueue[mqttQHead].payload[copy] = 0;
    mqttQueue[mqttQHead].len = copy;
    mqttQHead = next;
  }
  xSemaphoreGive(mqttQMutex);
}

void mqttPublishMsg(const RS485Msg& m) {
  if (!mqtt.connected()) return;
  // Build the JSON with snprintf + a direct transcode -- no JsonDocument heap
  // allocation in this hot path. One worst-case buffer holds the whole message
  // so it is NEVER truncated: the fixed prefix is <=59 bytes, the command body
  // is at most MSG_MAX_BYTES*3 (every flap byte -> a 3-byte UTF-8 glyph), and
  // the "} suffix is 2 -- all inside MSG_MAX_BYTES*3 + 80. A single buffer (vs a
  // separate transcode scratch) keeps stack use modest: mqttPublishMsg runs on
  // taskRS485 and taskNetwork, both of which have only a 6KB stack.
  char buf[MSG_MAX_BYTES * 3 + 80];
  int pre = snprintf(buf, sizeof(buf),
    "{\"ts\":%lu,\"wt\":\"%s\",\"command\":\"", m.timestamp, m.wallTime);
  if (pre < 0) return;
  // Transcode the frame straight into buf after the prefix: escapes "/\, strips
  // line breaks, maps unprintable bytes to a space, and shows high Windows-1252
  // bytes as their real glyph (shared with the web monitor). Reserve 3 bytes for
  // the closing "} and NUL.
  size_t body = flapToJsonUtf8((const char*)m.data, m.len,
                               buf + pre, sizeof(buf) - (size_t)pre - 3, ' ');
  size_t n = (size_t)pre + body;
  buf[n++] = '"';
  buf[n++] = '}';
  buf[n]   = '\0';
  char _t[80];
  snprintf(_t, sizeof(_t), "%s/%s", cfg.mqttPrefix, m.dir=='R'?"rx":"tx");
  mqttEnqueue(_t, buf, n);
}

void mqttPublishSFEvent(const char* event, const char* payload) {
  char _t[80];
  snprintf(_t,sizeof(_t),"%s/flap/%s",cfg.mqttPrefix,event);
  mqttEnqueue(_t, payload, strlen(payload));
}

void mqttPublishStatus() {
  if (!mqtt.connected()) return;
  char timeBuf[24];
  rtcFormatTime(timeBuf, sizeof(timeBuf));
  char ip[20];
  IPAddress lip = WiFi.localIP();
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", lip[0], lip[1], lip[2], lip[3]);
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  // Full diagnostic set -- mirrors the [WDG] heartbeat so Home Assistant can
  // surface the same health signals (min heap, largest block, fragmentation,
  // parse rejects, and per-task stack high-water marks).
  unsigned freeHeap = ESP.getFreeHeap();
  unsigned minHeap  = ESP.getMinFreeHeap();
  unsigned maxBlk   = ESP.getMaxAllocHeap();
  unsigned frag     = freeHeap ? (unsigned)(100 - (maxBlk * 100UL / freeHeap)) : 0;
  unsigned s485 = hTaskRS485 ? uxTaskGetStackHighWaterMark(hTaskRS485) : 0;
  unsigned sWeb = hTaskWeb   ? uxTaskGetStackHighWaterMark(hTaskWeb)   : 0;
  unsigned sNet = hTaskNet   ? uxTaskGetStackHighWaterMark(hTaskNet)   : 0;
  unsigned sOta = hTaskOTA   ? uxTaskGetStackHighWaterMark(hTaskOTA)   : 0;
  unsigned sRtc = hTaskRTC   ? uxTaskGetStackHighWaterMark(hTaskRTC)   : 0;
  char buf[640];
  size_t n = (size_t)snprintf(buf, sizeof(buf),
    "{\"uptime\":%lu,\"rx\":%lu,\"tx\":%lu,\"rej\":%lu,\"modules\":%d,"
    "\"time\":\"%s\",\"ntpSynced\":%s,\"heap\":%u,\"minheap\":%u,"
    "\"maxblk\":%u,\"frag\":%u,\"rssi\":%d,\"wifi\":%s,"
    "\"stk485\":%u,\"stkweb\":%u,\"stknet\":%u,\"stkota\":%u,\"stkrtc\":%u,"
    "\"ip\":\"%s\",\"url\":\"http://%s/\",\"version\":\"%s\","
    "\"maintenance\":%s,\"quiet\":%s}",
    millis()/1000, rxCount, txCount, sfParseRejects, sfModuleCount,
    timeBuf, ntpSynced?"true":"false", freeHeap, minHeap,
    maxBlk, frag, rssi, (WiFi.status()==WL_CONNECTED)?"true":"false",
    s485, sWeb, sNet, sOta, sRtc,
    ip, ip, FW_VERSION, gMaintenanceMode?"true":"false", gQuietTime?"true":"false");
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  char _t[80];
  snprintf(_t, sizeof(_t), "%s/status", cfg.mqttPrefix);
  mqttEnqueue(_t, buf, n);
}

// Assemble the display string in module-id order across the configured grid,
// from the SAME source as the live-display wall: gWallChars, the last flap byte
// transmitted to each grid cell. This mirrors exactly what the gateway sent --
// independent of the module registry, so it is unaffected by provisioning state
// or stale-probe eviction. A provisioned cell never written reads '?'.
void mqttPublishDisplayState() {
  if (!mqtt.connected()) return;
  int cells = (int)cfg.gridRows * (int)cfg.gridCols;
  if (cells < 1) cells = 1;
  if (cells > MAX_MODULES) cells = MAX_MODULES;
  // Each cell is one glyph; a Windows-1252 high byte (euro/accent) expands to up
  // to 3 UTF-8 bytes, so size the buffer for the worst case.
  static char str[MAX_MODULES * 3 + 1];   // static: single-caller (taskNetwork)
  int outLen = 0;
  if (xSemaphoreTake(sfMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (int id = 0; id < cells; id++) {
      uint8_t uc = (id < (int)sizeof(gWallChars)) ? (uint8_t)gWallChars[id] : 0;
      if (!(uc && isFlapByte(uc))) {          // nothing valid sent to this cell
        SFModule* m = sfFindById((uint8_t)id);
        uc = (m && m->provisioned) ? (uint8_t)'?' : (uint8_t)' ';
      }
      outLen += flapByteToUtf8(uc, str + outLen);       // ASCII -> 1 byte, high -> UTF-8
    }
    xSemaphoreGive(sfMutex);
  }
  str[outLen] = '\0';
  char _t[80];
  snprintf(_t, sizeof(_t), "%s/display/state", cfg.mqttPrefix);
  mqttEnqueue(_t, str, outLen);
}

// Publish the HA-facing entity state topics (maintenance + quiet switches, and
// the display string). Called on connect, on any toggle, and when display
// tracking changes. No-op unless HA integration is enabled.
void mqttPublishStateTopics() {
  if (!mqtt.connected() || !cfg.haEnabled) return;
  char t[80];
  snprintf(t, sizeof(t), "%s/maintenance/state", cfg.mqttPrefix);
  mqttEnqueue(t, gMaintenanceMode ? "ON" : "OFF", gMaintenanceMode ? 2 : 3);
  snprintf(t, sizeof(t), "%s/quiet/state", cfg.mqttPrefix);
  mqttEnqueue(t, gQuietTime ? "ON" : "OFF", gQuietTime ? 2 : 3);
  mqttPublishDisplayState();
}

// Publish (or remove) Home Assistant MQTT discovery configs. When enable is
// true, retained config messages are sent under homeassistant/<comp>/<node>/...
// so HA auto-creates the entities; when false, empty retained payloads are sent
// to the same topics to delete them. Uses HA's abbreviated discovery keys and a
// shared base-topic (~) to keep each payload within MQTT_BUF_SIZE. All entities
// share one device block (linked by the gateway's chip id) so they group under a
// single HA device. Diagnostic sensors read fields from the <prefix>/status JSON.
void haPublishDiscovery(bool enable) {
  if (!mqtt.connected()) return;
  char node[24];
  snprintf(node, sizeof(node), "sfgw_%08X", (uint32_t)ESP.getEfuseMac());
  const char* pfx = cfg.mqttPrefix;
  char topic[160];
  char pl[MQTT_BUF_SIZE];

  // Shared fragments: device block + availability. avty_t uses <prefix>/availability.
  // Kept compact; HA merges the device block across entities by identifier.
  char dev[200];
  snprintf(dev, sizeof(dev),
    "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Split-Flap Gateway\",\"mf\":\"Anthropic SFGW\",\"mdl\":\"ESP32-S3 RS485\",\"sw\":\"%s\"},"
    "\"avty_t\":\"%s/availability\"", node, FW_VERSION, pfx);

  // Helper macro-like lambda is not available; emit each entity inline.
  // 1) Display text entity: set/echo the whole display string.
  snprintf(topic, sizeof(topic), "homeassistant/text/%s/display/config", node);
  if (enable) {
    snprintf(pl, sizeof(pl),
      "{\"name\":\"Display\",\"uniq_id\":\"%s_display\",\"cmd_t\":\"%s/display/set\","
      "\"stat_t\":\"%s/display/state\",\"max\":255,%s}", node, pfx, pfx, dev);
    mqttEnqueue(topic, pl, strlen(pl));
  } else mqttEnqueue(topic, "", 0);

  // 2) Maintenance mode switch.
  snprintf(topic, sizeof(topic), "homeassistant/switch/%s/maintenance/config", node);
  if (enable) {
    snprintf(pl, sizeof(pl),
      "{\"name\":\"Maintenance Mode\",\"uniq_id\":\"%s_maint\",\"cmd_t\":\"%s/maintenance/set\","
      "\"stat_t\":\"%s/maintenance/state\",\"ic\":\"mdi:wrench\",%s}", node, pfx, pfx, dev);
    mqttEnqueue(topic, pl, strlen(pl));
  } else mqttEnqueue(topic, "", 0);

  // 3) Quiet Time switch.
  snprintf(topic, sizeof(topic), "homeassistant/switch/%s/quiet/config", node);
  if (enable) {
    snprintf(pl, sizeof(pl),
      "{\"name\":\"Quiet Time\",\"uniq_id\":\"%s_quiet\",\"cmd_t\":\"%s/quiet/set\","
      "\"stat_t\":\"%s/quiet/state\",\"ic\":\"mdi:volume-off\",%s}", node, pfx, pfx, dev);
    mqttEnqueue(topic, pl, strlen(pl));
  } else mqttEnqueue(topic, "", 0);

  // 4) Diagnostic sensors -- all read the <prefix>/status JSON via value_template.
  //    Mirrors the [WDG] heartbeat: heap/min/maxblk/frag, parse rejects, the five
  //    per-task stack high-water marks, plus connectivity and identity.
  struct DiagS { const char* obj; const char* name; const char* fld; const char* unit; const char* dc; const char* ic; };
  static const DiagS diags[] = {
    {"modules", "Modules",        "modules", NULL,  NULL,              "mdi:view-grid"},
    {"uptime",  "Uptime",         "uptime",  "s",   "duration",        NULL},
    {"heap",    "Free Heap",      "heap",    "B",   NULL,              "mdi:memory"},
    {"minheap", "Min Free Heap",  "minheap", "B",   NULL,              "mdi:memory"},
    {"maxblk",  "Max Alloc Block","maxblk",  "B",   NULL,              "mdi:memory"},
    {"frag",    "Heap Fragmentation","frag", "%",   NULL,              "mdi:chart-donut"},
    {"rssi",    "WiFi Signal",    "rssi",    "dBm", "signal_strength", NULL},
    {"rx",      "Frames Received","rx",      NULL,  NULL,              "mdi:download-network"},
    {"tx",      "Frames Sent",    "tx",      NULL,  NULL,              "mdi:upload-network"},
    {"rej",     "Parse Rejects",  "rej",     NULL,  NULL,              "mdi:alert-circle"},
    {"stk485",  "Stack RS485",    "stk485",  "B",   NULL,              "mdi:layers"},
    {"stkweb",  "Stack Web",      "stkweb",  "B",   NULL,              "mdi:layers"},
    {"stknet",  "Stack Network",  "stknet",  "B",   NULL,              "mdi:layers"},
    {"stkota",  "Stack OTA",      "stkota",  "B",   NULL,              "mdi:layers"},
    {"stkrtc",  "Stack RTC",      "stkrtc",  "B",   NULL,              "mdi:layers"},
    {"ip",      "IP Address",     "ip",      NULL,  NULL,              "mdi:ip-network"},
    {"url",     "Gateway URL",    "url",     NULL,  NULL,              "mdi:web"},
    {"version", "Firmware",       "version", NULL,  NULL,              "mdi:chip"},
  };
  for (unsigned i = 0; i < sizeof(diags)/sizeof(diags[0]); i++) {
    const DiagS& d = diags[i];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", node, d.obj);
    if (enable) {
      int p = snprintf(pl, sizeof(pl),
        "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s/status\","
        "\"val_tpl\":\"{{ value_json.%s }}\",\"ent_cat\":\"diagnostic\"",
        d.name, node, d.obj, pfx, d.fld);
      if (d.unit) p += snprintf(pl+p, sizeof(pl)-p, ",\"unit_of_meas\":\"%s\"", d.unit);
      if (d.dc)   p += snprintf(pl+p, sizeof(pl)-p, ",\"dev_cla\":\"%s\"", d.dc);
      if (d.ic)   p += snprintf(pl+p, sizeof(pl)-p, ",\"ic\":\"%s\"", d.ic);
      snprintf(pl+p, sizeof(pl)-p, ",%s}", dev);
      mqttEnqueue(topic, pl, strlen(pl));
    } else mqttEnqueue(topic, "", 0);
  }

  printf("[HA] Discovery %s (node %s)\n", enable ? "published" : "removed", node);
}
//   <prefix>/flap/set      {"id":5,"char":"A"}  or  {"id":5,"index":3}
//                          {"id":-1,"text":"HELLO","start":0}  multi-module text
//   <prefix>/flap/home     {"id":5}  or  {"id":-1}  (broadcast)
//   <prefix>/flap/provision {"sn":"AABBCC...","id":5}
//   <prefix>/flap/flapconfig {"id":5,"flapCount":40,"charSet":" ABC..."}  (firmware
//                          v31+; id=-1 broadcasts; or target "sn"; flapCount and
//                          charSet are independent and optional)
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Control topics are handled FIRST, before the maintenance-mode gate, so the
  // maintenance and quiet switches remain reachable over MQTT/HA even while
  // maintenance mode is on (otherwise you could never turn it back off remotely).
  if (length < MQTT_BUF_SIZE) {
    static char cbuf[64];
    size_t cl = length < sizeof(cbuf) - 1 ? length : sizeof(cbuf) - 1;
    memcpy(cbuf, payload, cl); cbuf[cl] = 0;
    // Trim trailing whitespace/newline and detect on/off
    while (cl && (cbuf[cl-1]=='\n'||cbuf[cl-1]=='\r'||cbuf[cl-1]==' ')) cbuf[--cl]=0;
    bool on = (strcasecmp(cbuf, "ON") == 0 || strcasecmp(cbuf, "true") == 0 ||
               strcasecmp(cbuf, "1") == 0);
    char ctl[80];
    snprintf(ctl, sizeof(ctl), "%s/maintenance/set", cfg.mqttPrefix);
    if (strcmp(topic, ctl) == 0) {
      gMaintenanceMode = on;
      printf("[MAINT] Maintenance mode %s (MQTT)\n", on ? "ENABLED" : "disabled");
      mqttPublishStateTopics();
      return;
    }
    snprintf(ctl, sizeof(ctl), "%s/quiet/set", cfg.mqttPrefix);
    if (strcmp(topic, ctl) == 0) {
      // The quiet SCHEDULE is authoritative inside its window: refuse an external
      // OFF (a retained message, an HA switch, an automation) so it can't fight
      // the schedule -- that fight caused quiet to flap and its resync churn to
      // break OTA uploads. Republish state so the sender re-syncs to ON.
      if (!on && quietSchedInWindow()) {
        printf("[QUIET] MQTT quiet/set OFF ignored -- schedule active (in window)\n");
        mqttPublishStateTopics();
        return;
      }
      sfSetQuietTime(on);
      mqttPublishStateTopics();
      return;
    }
  }
  // Maintenance mode: ignore all externally-originated DISPLAY commands. Nothing
  // from MQTT is relayed to the bus while this is on; only the gateway's own web
  // UI / REST API can drive the display.
  if (gMaintenanceMode) {
    DBG("[MQTT] ignored (maintenance mode): %s\n", topic);
    return;
  }
  if (length >= MQTT_BUF_SIZE) return;
  // static (not stack): mqttCallback is invoked only from mqtt.loop() in
  // taskNetwork (single caller, no reentrancy). Three ~768-byte stack buffers
  // here plus the rs485Send/mqttPublishMsg call chain would push taskNetwork's
  // 6KB stack toward overflow -- the same failure mode that crashed taskRS485.
  static char buf[MQTT_BUF_SIZE + 1];
  memcpy(buf, payload, length);
  buf[length] = 0;

  // Compare topic using char* to avoid heap String allocation on every message
  char sendTopic[80], setTopic[80], homeTopic[80], provTopic[80], flapCfgTopic[80];
  snprintf(sendTopic, sizeof(sendTopic), "%s/send",           cfg.mqttPrefix);
  snprintf(setTopic,  sizeof(setTopic),  "%s/flap/set",       cfg.mqttPrefix);
  snprintf(homeTopic, sizeof(homeTopic), "%s/flap/home",      cfg.mqttPrefix);
  snprintf(provTopic, sizeof(provTopic), "%s/flap/provision", cfg.mqttPrefix);
  snprintf(flapCfgTopic, sizeof(flapCfgTopic), "%s/flap/flapconfig", cfg.mqttPrefix);
  char dispTopic[80];
  snprintf(dispTopic, sizeof(dispTopic), "%s/display/set", cfg.mqttPrefix);

  // HA display text entity: a plain string to show across the whole display,
  // starting at module 0. Handled before JSON parse since the payload is text.
  if (strcmp(topic, dispTopic) == 0) {
    // strip a single trailing newline if present
    size_t L = strlen(buf);
    if (L && (buf[L-1]=='\n'||buf[L-1]=='\r')) buf[L-1]=0;
    DBG("[MQTT] display set: %s\n", buf);
    { char cd[MSG_MAX_BYTES]; snprintf(cd, sizeof(cd), "display %s", buf); ringPushCommand('M', cd); }
    sfSendText(0, buf, false);
    mqttPublishDisplayState();
    return;
  }

  // Handle the raw send topic before attempting JSON parse:
  // Accept either a plain ASCII frame ("m9h\n") or JSON ({"data":"m9h\n"}).
  if (strcmp(topic, sendTopic) == 0) {
    const char* d = nullptr;
    bool raw = false;   // optional {"raw":true} -> send verbatim, bypass sanitization
    // Sized for long commands (e.g. a full restore) sent as a plain frame.
    // static: see note on buf above (single-caller context, stack pressure).
    static char plainBuf[TX_MAX_BYTES + 1];
    if (buf[0] == '{') {
      // Try JSON
      JsonDocument doc;
      if (deserializeJson(doc, buf) == DeserializationError::Ok) {
        d = doc["data"] | "";
        raw = doc["raw"] | false;
      }
    } else {
      // Plain string -- use as-is
      strlcpy(plainBuf, buf, sizeof(plainBuf));
      d = plainBuf;
    }
    if (d && d[0]) {
      { char _dbg[MSG_MAX_BYTES]; size_t _dl = strlen(d);
        if (_dl > 0 && (d[_dl-1]=='\n'||d[_dl-1]=='\r')) _dl--;
        memcpy(_dbg, d, _dl); _dbg[_dl] = '\0';
        DBG("[MQTT->BUS] %s\n", _dbg); }
      static uint8_t outBuf[TX_MAX_BYTES];  // static: see note on buf above
      size_t  outLen = min(strlen(d), (size_t)TX_MAX_BYTES);
      memcpy(outBuf, d, outLen);
      { char cd[MSG_MAX_BYTES]; snprintf(cd, sizeof(cd), "send %s", d); ringPushCommand('M', cd); }
      rs485Send(outBuf, outLen, raw);
    }
    return;
  }

  // All other topics require JSON
  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) return;

  if (strcmp(topic, setTopic) == 0) {
    int id = doc["id"] | -99;
    if (id == -99) return;
    // Text mode: send a string across sequential modules
    const char* text = doc["text"] | "";
    if (strlen(text) > 0) {
      int start = doc["start"] | id;
      { char cd[MSG_MAX_BYTES]; snprintf(cd, sizeof(cd), "text@%d %s", start, text); ringPushCommand('M', cd); }
      sfSendText(start, text, false);
      return;
    }
    // Single char (UTF-8 -> one Windows-1252 byte for euro/accented glyphs)
    const char* ch = doc["char"] | "";
    if (strlen(ch) > 0) {
      char enc[8]; utf8ToFlap(ch, enc, sizeof(enc));
      DBG("[API] show char on module %d\n", id);
      if (enc[0]) sfSendChar(id, enc[0]);
      return;
    }
    // Index
    if (doc["index"].is<int>()) { sfSendIndex(id, doc["index"].as<int>()); }
  }
  else if (strcmp(topic, homeTopic) == 0) {
    int id = doc["id"] | -1;
    sfHome(id < 0 ? -1 : id);
  }
  else if (strcmp(topic, provTopic) == 0) {
    const char* sn = doc["sn"] | "";
    int newId = doc["id"] | -1;
  DBG("[API] provision SN %s -> ID %d\n", sn, newId);
    if (strlen(sn) > 0 && newId >= 0) sfProvision(sn, newId);
  }
  // Configure flap set ('N', firmware v31+). flapCount (1-64) and charSet are
  // independent and optional; at least one is required. Target by id (id=-1
  // broadcasts m*N) or by serial number (sn). The UTF-8 charSet is transcoded to
  // Windows-1252 (euro/accented glyphs become one flap byte each); an unmappable
  // char or an over-long / out-of-range request is dropped.
  //   {"id":5,"flapCount":40,"charSet":" ABC..."}   or  {"sn":"AABB...","charSet":"..."}
  else if (strcmp(topic, flapCfgTopic) == 0) {
    const char* sn      = doc["sn"]        | "";
    int   id            = doc["id"]        | -99;
    int   flapCount     = doc["flapCount"] | 0;
    const char* charSet = doc["charSet"]   | "";
    bool hasCount = (flapCount != 0);
    bool hasChars = (charSet[0] != 0);
    if ((!hasCount && !hasChars) ||
        (hasCount && (flapCount < 1 || flapCount > SF_MAX_FLAPS))) return;
    char enc[SF_MAX_FLAPS * 4 + 4];
    if (hasChars) {
      bool allMapped = true;
      size_t n = utf8ToFlap(charSet, enc, sizeof(enc), &allMapped);
      if (!allMapped || n == 0 || n > SF_MAX_FLAPS) return;   // unmappable / empty / too long
    }
    int   reqCount = hasCount ? flapCount : 0;
    const char* reqChars = hasChars ? enc : nullptr;
    DBG("[MQTT] flap config %s count=%d chars='%s'\n", sn[0] ? sn : "by-id", flapCount, charSet);
    if (sn[0])               sfSetFlapConfigBySN(sn, reqCount, reqChars);
    else if (id >= -1 && id <= 254) sfSetFlapConfig(id, reqCount, reqChars);
  }
}

// Called once from setup(). Initialises MQTT settings that allocate heap
// and must not be called repeatedly.
void mqttInit() {
  mqttWifiClient.setTimeout(5000);  // 5s TCP connect timeout
  mqtt.setClient(mqttWifiClient);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(MQTT_BUF_SIZE);
  mqtt.setKeepAlive(15);  // 15s keepalive -- detect dead connections faster
  mqtt.setSocketTimeout(5);  // 5s socket timeout
  if (strlen(cfg.mqttHost)) mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
}

void mqttConnect() {
  if (!strlen(cfg.mqttHost)) return;
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "splitflap-%08X", (uint32_t)ESP.getEfuseMac());
  printf("[MQTT] Connecting to %s:%d...\n", cfg.mqttHost, cfg.mqttPort);
  // Last Will & Testament: the broker publishes "offline" to <prefix>/availability
  // (retained) if we drop without a clean disconnect. HA uses this to mark every
  // entity unavailable. We publish "online" ourselves on connect (the birth).
  char availT[80];
  snprintf(availT, sizeof(availT), "%s/availability", cfg.mqttPrefix);
  bool ok = strlen(cfg.mqttUser)
    ? mqtt.connect(clientId, cfg.mqttUser, cfg.mqttPass, availT, 0, true, "offline")
    : mqtt.connect(clientId, NULL, NULL, availT, 0, true, "offline");
  if (ok) {
    printf("[MQTT] Connected\n");
    // Birth: mark available (retained).
    mqtt.publish(availT, "online", true);
    // Use char arrays not String to avoid heap fragmentation
    char t[80];
    snprintf(t,sizeof(t),"%s/send",           cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/flap/set",       cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/flap/home",      cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/flap/provision", cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/flap/flapconfig",cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/display/set",    cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/maintenance/set",cfg.mqttPrefix); mqtt.subscribe(t);
    snprintf(t,sizeof(t),"%s/quiet/set",      cfg.mqttPrefix); mqtt.subscribe(t);
    // Home Assistant integration: publish discovery + initial entity state.
    if (cfg.haEnabled) {
      haPublishDiscovery(true);
      mqttPublishStateTopics();
    }
  } else {
    int st = mqtt.state();
    const char* why;
    switch (st) {                       // PubSubClient state codes
      case -4: why = "timeout";               break;
      case -3: why = "connection lost";       break;
      case -2: why = "connect failed (TCP)";  break;
      case -1: why = "disconnected";          break;
      case  1: why = "bad protocol";          break;
      case  2: why = "bad client id";         break;
      case  3: why = "server unavailable";    break;
      case  4: why = "bad credentials";       break;
      case  5: why = "not authorized";        break;
      default: why = "unknown";               break;
    }
    printf("[MQTT] Failed rc=%d (%s)\n", st, why);
  }
}
