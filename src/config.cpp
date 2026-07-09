#include "gateway.h"


// config.cpp -- runtime configuration persisted in NVS (Preferences).
// Defaults live in cfgSetDefaults(); loadConfig()/saveConfig() move the struct
// to and from the "splitflap" NVS namespace. Called from setup() and the
// /api/config/* handlers.
void cfgSetDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.mqttPort      = DEFAULT_MQTT_PORT;
  cfg.rs485Baud     = DEFAULT_BAUD;
  cfg.rs485DataBits = 8;
  cfg.rs485Parity   = 0;
  cfg.rs485StopBits = 1;
  strlcpy(cfg.mqttPrefix, DEFAULT_MQTT_PREFIX, sizeof(cfg.mqttPrefix));
  strlcpy(cfg.posixTZ, "UTC0", sizeof(cfg.posixTZ));
  strlcpy(cfg.ntpServer, DEFAULT_NTP_SERVER, sizeof(cfg.ntpServer));
  cfg.gridRows = 1;
  cfg.gridCols = 16;
  cfg.serialDebug = false;
  gSerialDebug    = false;
  cfg.haEnabled   = false;
  strlcpy(cfg.otaPassword, "", sizeof(cfg.otaPassword));
  strlcpy(gPosixTZ,    "UTC0", sizeof(gPosixTZ));
  // v3.0 defaults
  strlcpy(cfg.companionUrl, "", sizeof(cfg.companionUrl));
  cfg.quietSchedEnabled = false;
  strlcpy(cfg.quietStart, "22:00", sizeof(cfg.quietStart));
  strlcpy(cfg.quietEnd,   "07:00", sizeof(cfg.quietEnd));
  cfg.quietDays = 0x7F;  // all days
  cfg.quietTzOffsetMin = 0;   // captured from the browser on Save Schedule
}
// Migrate settings from old NVS namespace "rs485gw" to "splitflap".
// Runs once after firmware update; no-op on subsequent boots.
void loadConfig() {
  prefs.begin("splitflap", true);
  strlcpy(cfg.wifiSSID,   prefs.getString("wSSID",  "").c_str(), sizeof(cfg.wifiSSID));
  strlcpy(cfg.wifiPass,   prefs.getString("wPASS",  "").c_str(), sizeof(cfg.wifiPass));
  strlcpy(cfg.mqttHost,   prefs.getString("mqHost", "").c_str(), sizeof(cfg.mqttHost));
  cfg.mqttPort          = prefs.getInt   ("mqPort",  DEFAULT_MQTT_PORT);
  strlcpy(cfg.mqttUser,   prefs.getString("mqUser", "").c_str(), sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass,   prefs.getString("mqPass", "").c_str(), sizeof(cfg.mqttPass));
  strlcpy(cfg.mqttPrefix, prefs.getString("mqPfx",  DEFAULT_MQTT_PREFIX).c_str(), sizeof(cfg.mqttPrefix));
  cfg.rs485Baud       = prefs.getULong("baud",    DEFAULT_BAUD);
  cfg.rs485DataBits   = prefs.getUChar("dbits",   8);
  cfg.rs485Parity     = prefs.getUChar("parity",  0);
  cfg.rs485StopBits   = prefs.getUChar("sbits",   1);
  strlcpy(cfg.posixTZ, prefs.getString("tz", "UTC0").c_str(), sizeof(cfg.posixTZ));
  strlcpy(cfg.ntpServer, prefs.getString("ntp", DEFAULT_NTP_SERVER).c_str(), sizeof(cfg.ntpServer));
  cfg.gridRows = (uint8_t)prefs.getInt("gRows", 1);
  cfg.gridCols = (uint8_t)prefs.getInt("gCols", 16);
  if (cfg.gridRows < 1) cfg.gridRows = 1;
  if (cfg.gridCols < 1) cfg.gridCols = 1;
  cfg.serialDebug = prefs.getBool("dbgSerial", false);
  gSerialDebug    = cfg.serialDebug;
  cfg.haEnabled   = prefs.getBool("haEnabled", false);
  strlcpy(cfg.otaPassword, prefs.getString("otaPass", "").c_str(), sizeof(cfg.otaPassword));
  // v3.0
  strlcpy(cfg.companionUrl, prefs.getString("compUrl", "").c_str(), sizeof(cfg.companionUrl));
  cfg.quietSchedEnabled = prefs.getBool("qsEn", false);
  strlcpy(cfg.quietStart, prefs.getString("qsStart", "22:00").c_str(), sizeof(cfg.quietStart));
  strlcpy(cfg.quietEnd,   prefs.getString("qsEnd",   "07:00").c_str(), sizeof(cfg.quietEnd));
  cfg.quietDays = prefs.getUChar("qsDays", 0x7F);
  cfg.quietTzOffsetMin = prefs.getShort("qsTzOff", 0);
  strlcpy(gPosixTZ, cfg.posixTZ, sizeof(gPosixTZ));
  setenv("TZ", gPosixTZ, 1);
  tzset();
  prefs.end();
}

void saveConfig() {
  prefs.begin("splitflap", false);
  prefs.putString("wSSID",  cfg.wifiSSID);
  prefs.putString("ntp",    cfg.ntpServer);
  prefs.putInt   ("gRows",  cfg.gridRows);
  prefs.putInt   ("gCols",  cfg.gridCols);
  prefs.putString("wPASS",  cfg.wifiPass);
  prefs.putString("mqHost", cfg.mqttHost);
  prefs.putInt   ("mqPort", cfg.mqttPort);
  prefs.putString("mqUser", cfg.mqttUser);
  prefs.putString("mqPass", cfg.mqttPass);
  prefs.putString("mqPfx",  cfg.mqttPrefix);
  prefs.putULong ("baud",   cfg.rs485Baud);
  prefs.putUChar ("dbits",  cfg.rs485DataBits);
  prefs.putUChar ("parity", cfg.rs485Parity);
  prefs.putUChar ("sbits",  cfg.rs485StopBits);
  prefs.putString("tz",     cfg.posixTZ);
  prefs.putBool  ("dbgSerial", cfg.serialDebug);
  prefs.putBool  ("haEnabled", cfg.haEnabled);
  prefs.putString("otaPass",   cfg.otaPassword);
  // v3.0
  prefs.putString("compUrl",   cfg.companionUrl);
  prefs.putBool  ("qsEn",      cfg.quietSchedEnabled);
  prefs.putString("qsStart",   cfg.quietStart);
  prefs.putString("qsEnd",     cfg.quietEnd);
  prefs.putUChar ("qsDays",    cfg.quietDays);
  prefs.putShort ("qsTzOff",   cfg.quietTzOffsetMin);
  prefs.end();
}
