#include "gateway.h"
#include "perf.h"

// perf.cpp -- see perf.h. Fixed tables, no allocation: the profiler must not be able to cause
// the thing it measures. The request tables are written only by taskWeb (the one task that
// handles requests) and read by a handler on the same task, so there is nothing to lock. The
// loop-gap slots are written by one task each (aligned 32-bit stores) and only read here.

#define PERF_EP_MAX      40
#define PERF_CLIENT_MAX   8
#define PERF_URI_LEN     36

struct PerfEp     { char uri[PERF_URI_LEN]; uint32_t count, totalMs, maxMs, lowHeap; };
struct PerfClient { uint32_t ip, count, totalMs; };

static PerfEp     gEp[PERF_EP_MAX];
static PerfClient gCl[PERF_CLIENT_MAX];
static uint32_t   gSinceMs = 0, gReqs = 0, gBusyMs = 0, gWaitMs = 0, gOther = 0;

static volatile uint32_t gLastReqMs = 0;             // millis() of the last completed request
uint32_t perfSinceLastRequestMs() { return millis() - gLastReqMs; }

static volatile uint32_t gTickMs[PERF_T_COUNT];      // millis() of each task's last pass
static volatile uint32_t gGapMs[PERF_T_COUNT];       // longest gap between passes, this window
static const char* const TASK_NAMES[PERF_T_COUNT] = { "rs485", "web", "network", "rtc" };

void perfLoopTick(int task) {
  if (task < 0 || task >= PERF_T_COUNT) return;
  const uint32_t now = millis(), last = gTickMs[task];
  gTickMs[task] = now;
  if (last && now - last > gGapMs[task]) gGapMs[task] = now - last;
}

void perfReset() {
  memset(gEp, 0, sizeof(gEp));
  memset(gCl, 0, sizeof(gCl));
  gSinceMs = millis(); gReqs = 0; gBusyMs = 0; gWaitMs = 0; gOther = 0;
  for (int i = 0; i < PERF_T_COUNT; i++) gGapMs[i] = 0;
}

void perfNoteRequest(const char* uri, uint32_t ip, uint32_t ms, uint32_t newLowHeap) {
  if (!gSinceMs) gSinceMs = millis();
  gLastReqMs = millis();
  gReqs++; gBusyMs += ms;

  PerfEp* e = NULL;
  for (int i = 0; i < PERF_EP_MAX; i++) {
    if (!gEp[i].uri[0]) { e = &gEp[i]; strlcpy(e->uri, uri, sizeof(e->uri)); break; }
    if (strncmp(gEp[i].uri, uri, PERF_URI_LEN - 1) == 0) { e = &gEp[i]; break; }
  }
  if (e) {
    e->count++; e->totalMs += ms; if (ms > e->maxMs) e->maxMs = ms;
    if (newLowHeap && (!e->lowHeap || newLowHeap < e->lowHeap)) e->lowHeap = newLowHeap;
  }
  else gOther++;                                  // table full: counted, not attributed

  PerfClient* c = NULL;
  for (int i = 0; i < PERF_CLIENT_MAX; i++) {
    if (!gCl[i].count) { c = &gCl[i]; c->ip = ip; break; }
    if (gCl[i].ip == ip) { c = &gCl[i]; break; }
  }
  if (c) { c->count++; c->totalMs += ms; }
}

void perfNoteWait(uint32_t ms) {
  if (!gSinceMs) gSinceMs = millis();
  gWaitMs += ms;
}

void perfEmit(void (*emit)(const char* chunk)) {
  char b[240];
  const uint32_t win = gSinceMs ? (millis() - gSinceMs) : 0;
  snprintf(b, sizeof(b),
    "{\"ok\":true,\"windowMs\":%lu,\"requests\":%lu,\"handlerMs\":%lu,\"silentClientMs\":%lu,"
    "\"unattributed\":%lu,\"heap\":%lu,\"minHeap\":%lu,\"maxBlock\":%lu,\"loopGapMs\":{",
    (unsigned long)win, (unsigned long)gReqs, (unsigned long)gBusyMs, (unsigned long)gWaitMs,
    (unsigned long)gOther, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
    (unsigned long)ESP.getMaxAllocHeap());
  emit(b);
  for (int i = 0; i < PERF_T_COUNT; i++) {
    snprintf(b, sizeof(b), "%s\"%s\":%lu", i ? "," : "", TASK_NAMES[i], (unsigned long)gGapMs[i]);
    emit(b);
  }
  emit("},\"endpoints\":[");
  bool first = true;
  for (int i = 0; i < PERF_EP_MAX && gEp[i].uri[0]; i++) {
    snprintf(b, sizeof(b), "%s{\"uri\":\"%s\",\"n\":%lu,\"ms\":%lu,\"max\":%lu,\"lowHeap\":%lu}",
             first ? "" : ",", gEp[i].uri, (unsigned long)gEp[i].count,
             (unsigned long)gEp[i].totalMs, (unsigned long)gEp[i].maxMs,
             (unsigned long)gEp[i].lowHeap);
    emit(b); first = false;
  }
  emit("],\"clients\":[");
  first = true;
  for (int i = 0; i < PERF_CLIENT_MAX && gCl[i].count; i++) {
    const uint32_t ip = gCl[i].ip;
    snprintf(b, sizeof(b), "%s{\"ip\":\"%u.%u.%u.%u\",\"n\":%lu,\"ms\":%lu}", first ? "" : ",",
             (unsigned)(ip & 255), (unsigned)((ip >> 8) & 255), (unsigned)((ip >> 16) & 255),
             (unsigned)(ip >> 24), (unsigned long)gCl[i].count, (unsigned long)gCl[i].totalMs);
    emit(b); first = false;
  }
  emit("]}");
}
