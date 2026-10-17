#include "gateway.h"

// bus.cpp -- the bus hold table. See bus.h for the contract.
// Tiny and lock-free on purpose: holds are taken from taskWeb, taskNetwork, the restore,
// probe and soak tasks, and read from all of them on every gated request. A missed update
// here costs one request answered a tick early or late, never a corrupt structure, so a
// mutex would buy nothing worth its deadlock risk against the bus and registry locks.

static BusHold gHolds[BUS_HOLD_SLOTS];

static bool expired(const BusHold& h, unsigned long now) {
  return h.untilMs != 0 && (long)(now - h.untilMs) >= 0;
}

bool busHold(const char* reason, BusLevel level, unsigned long leaseMs) {
  if (!reason || !reason[0]) return false;
  unsigned long now = millis();
  unsigned long until = leaseMs ? now + leaseMs : 0;
  int freeSlot = -1;
  for (int i = 0; i < BUS_HOLD_SLOTS; i++) {
    BusHold& h = gHolds[i];
    if (h.reason[0] && strcmp(h.reason, reason) == 0) {        // renew in place
      if (level > h.level) h.level = level;
      h.untilMs = until;
      return true;
    }
    if (!h.reason[0] || expired(h, now)) { if (freeSlot < 0) freeSlot = i; }
  }
  if (freeSlot < 0) { printf("[BUS] no free hold slot for '%s'\n", reason); return false; }
  BusHold& h = gHolds[freeSlot];
  strlcpy(h.reason, reason, sizeof(h.reason));
  h.level = level; h.sinceMs = now; h.untilMs = until;
  printf("[BUS] held by '%s' (%s%s)\n", reason, level == BUS_EXCLUSIVE ? "exclusive" : "busy",
         leaseMs ? "" : ", no expiry");
  return true;
}

void busRelease(const char* reason) {
  if (!reason) return;
  for (int i = 0; i < BUS_HOLD_SLOTS; i++) {
    if (gHolds[i].reason[0] && strcmp(gHolds[i].reason, reason) == 0) {
      printf("[BUS] released by '%s' after %lus\n", reason, (millis() - gHolds[i].sinceMs) / 1000UL);
      gHolds[i].reason[0] = 0;
    }
  }
}

static const BusHold* strongest() {
  unsigned long now = millis();
  const BusHold* best = NULL;
  for (int i = 0; i < BUS_HOLD_SLOTS; i++) {
    BusHold& h = gHolds[i];
    if (!h.reason[0]) continue;
    if (expired(h, now)) {                                        // reap a lapsed lease
      printf("[BUS] hold '%s' expired\n", h.reason);
      h.reason[0] = 0;
      continue;
    }
    if (!best || h.level > best->level) best = &h;
  }
  return best;
}

BusLevel busLevel()        { const BusHold* b = strongest(); return b ? (BusLevel)b->level : BUS_FREE; }
const char* busReason()    { const BusHold* b = strongest(); return b ? b->reason : ""; }
long busRemainingSec() {
  const BusHold* b = strongest();
  if (!b) return 0;
  if (!b->untilMs) return -1;
  long d = (long)(b->untilMs - millis()); return d > 0 ? (d + 999) / 1000 : 0;
}

void busStatusJson(char* out, size_t outLen) {
  const BusHold* b = strongest();
  int n = snprintf(out, outLen, "{\"level\":%d,\"reason\":\"%s\",\"remaining\":%ld,\"holds\":[",
                   b ? (int)b->level : 0, b ? b->reason : "", busRemainingSec());
  bool first = true;
  for (int i = 0; i < BUS_HOLD_SLOTS && n > 0 && (size_t)n < outLen; i++) {
    const BusHold& h = gHolds[i];
    if (!h.reason[0]) continue;
    long rem = h.untilMs ? (long)((h.untilMs - millis() + 999) / 1000) : -1;
    n += snprintf(out + n, outLen - n, "%s{\"reason\":\"%s\",\"level\":%d,\"remaining\":%ld}",
                  first ? "" : ",", h.reason, (int)h.level, rem);
    first = false;
  }
  if (n > 0 && (size_t)n < outLen) snprintf(out + n, outLen - n, "]}");
}
