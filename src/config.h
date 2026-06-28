// config.h -- runtime configuration: the GwConfig struct and load/save API.

#ifndef SFGW_CONFIG_H
#define SFGW_CONFIG_H

#include "common.h"

// Runtime configuration; the single instance is the global `cfg`. Defaults are
// set in cfgSetDefaults(); loadConfig()/saveConfig() persist it to the
// "splitflap" NVS namespace (config.cpp).
struct GwConfig {
  char          wifiSSID[64];
  char          wifiPass[64];
  char          mqttHost[64];
  int           mqttPort;
  char          mqttUser[64];
  char          mqttPass[64];
  char          mqttPrefix[32];
  unsigned long rs485Baud;
  uint8_t       rs485DataBits;
  uint8_t       rs485Parity;
  uint8_t       rs485StopBits;
  char          posixTZ[64];   // POSIX TZ string e.g. "EST5EDT,M3.2.0,M11.1.0"
  char          ntpServer[64]; // NTP server hostname (default pool.ntp.org)
  bool          serialDebug;   // enable verbose serial output
  bool          haEnabled;     // publish Home Assistant MQTT discovery + entity state
  char          otaPassword[32]; // OTA update password (blank = no auth)
  uint8_t       gridRows;      // visual display wall: rows (>=1)
  uint8_t       gridCols;      // visual display wall: columns (>=1)
};

// ---- owned globals (defined in globals.cpp) ----
extern GwConfig cfg;
extern Preferences prefs;

void cfgSetDefaults();
void loadConfig();
void saveConfig();

#endif // SFGW_CONFIG_H
