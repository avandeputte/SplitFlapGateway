// gateway.h -- umbrella header included by every .cpp in the project.
//
// Pulls in the shared kernel (common.h) and each subsystem's public API. A
// source file includes this one header and sees everything it may legally call.
//
// Source map:
//   common.h    libraries, board config macros, shared structs, global externs
//   globals.cpp single definition site for every shared/extern global
//   config.*    runtime configuration (GwConfig) persisted in NVS
//   rtc.*       PCF85063 real-time clock + NTP sync
//   rs485.*     RS-485 half-duplex bus: framing, sanitization, TX, monitor ring
//   modules.*   split-flap module registry, protocol commands, reply parser,
//               FATFS persistence
//   restore.*   restore-on-boot: replay a stored calibration backup, locking the bus
//   mqtt.*      MQTT client, outbound publish queue, Home Assistant discovery
//   web.*       HTTP server: dashboard page (web_ui.h) + REST API handlers
//   ota.*       firmware update: ArduinoOTA + browser upload
//   tasks.*     the FreeRTOS task loops (RS485 / RTC / Web / Network)
//   main.cpp    setup() boot sequence + loop() watchdog supervisor

#ifndef SFGW_GATEWAY_H
#define SFGW_GATEWAY_H

#include "common.h"
#include "config.h"
#include "charset.h"
#include "rtc.h"
#include "rs485.h"
#include "modules.h"
#include "capset.h"
#include "restore.h"
#include "mqtt.h"
#include "ota.h"
#include "web.h"
#include "tasks.h"

#endif // SFGW_GATEWAY_H
