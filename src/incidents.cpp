#include "gateway.h"

// incidents.cpp -- see incidents.h for what this is and why it is not just a counter.

static BusIncident* gRing  = NULL;
static volatile unsigned gWrite = 0;     // next slot to write
static volatile unsigned gTotal = 0;     // incidents ever recorded (gWrite wraps, this does not)
static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;

void incidentsInit() {
  if (!gRing) gRing = (BusIncident*) psramAlloc("incident log", sizeof(BusIncident) * INCIDENT_SLOTS);
}

void incidentLog(uint8_t id, char kind, char cmd, uint16_t bytes) {
  if (!gRing) return;
  BusIncident e;
  e.ms    = (uint32_t)millis();
  e.epoch = (uint32_t)rtcEpochNow();
  e.id    = id;
  e.kind  = kind;
  e.cmd   = cmd ? cmd : ' ';
  e.bytes = bytes;
  portENTER_CRITICAL(&gMux);
  gRing[gWrite % INCIDENT_SLOTS] = e;
  gWrite++;
  gTotal++;
  portEXIT_CRITICAL(&gMux);
}

unsigned incidentsTotal() { return gTotal; }

void incidentsClear() {
  portENTER_CRITICAL(&gMux);
  gWrite = 0; gTotal = 0;
  portEXIT_CRITICAL(&gMux);
}

void incidentsEmit(void (*emit)(const char* chunk), int max) {
  if (max < 1) max = 1;
  if (max > INCIDENT_SLOTS) max = INCIDENT_SLOTS;
  const unsigned total = gTotal;
  const unsigned held  = total < INCIDENT_SLOTS ? total : INCIDENT_SLOTS;
  const unsigned show  = (unsigned)max < held ? (unsigned)max : held;
  static char chunk[1400];
  size_t cl = 0;
  auto put = [&](const char* s) {
    size_t n = strlen(s);
    if (cl + n >= sizeof(chunk)) { chunk[cl] = 0; emit(chunk); cl = 0; }
    memcpy(chunk + cl, s, n); cl += n;
  };
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"total\":%u,\"held\":%u,\"shown\":%u,\"items\":[",
           total, held, show);
  put(buf);
  if (gRing) {
    for (unsigned k = 0; k < show; k++) {
      const BusIncident& e = gRing[(gWrite - 1 - k) % INCIDENT_SLOTS];   // newest first
      char idbuf[8];
      if (e.id == 255) strlcpy(idbuf, "null", sizeof(idbuf));
      else             snprintf(idbuf, sizeof(idbuf), "%u", (unsigned)e.id);
      snprintf(buf, sizeof(buf),
               "%s{\"ms\":%lu,\"ep\":%lu,\"id\":%s,\"kind\":\"%c\",\"cmd\":\"%c\",\"bytes\":%u}",
               k ? "," : "", (unsigned long)e.ms, (unsigned long)e.epoch, idbuf,
               e.kind, e.cmd, (unsigned)e.bytes);
      put(buf);
    }
  }
  put("]}");
  if (cl) { chunk[cl] = 0; emit(chunk); }
}
