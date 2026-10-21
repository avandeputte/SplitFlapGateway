// capset.h -- the set arithmetic behind GET /api/capabilities.
//
// Deliberately free of Arduino, so the firmware and the native regression test
// (tools/capset_test.cpp) compile the SAME code rather than two copies of a guess -- the same
// arrangement reel.h has on the Matrix Portal gateway, and for the same reason: this is fiddly
// logic (an intersection that must start from the first KNOWN reel, a range compressor with an
// off-by-one at every boundary) that cannot be exercised on a wall of real modules without
// physically rebuilding the wall.
//
// What it computes, given one reel per module:
//
//   union    every character SOME module can show. "Can this wall show a Z anywhere?"
//   common   every character EVERY module can show. "Can I lay this text across arbitrary
//            cells?" These genuinely differ: with module 1 carrying A-Z and module 2 carrying
//            0-9, the union is A-Z0-9 and the common set is EMPTY -- the wall cannot show
//            "HI42" wherever it likes, and only `common` says so.
//   ranges   the ids sharing a reel, compressed: "0-44,50". This is what keeps the response
//            small -- 45 modules with one reel between them are one entry, not 45 copies of the
//            same 64 characters.
//
// THREE TRANSLATIONS, because the wire is not the repertoire:
//   * r o y g b p w are the seven COLOUR flaps, not letters. Kept out of the character sets and
//     reported by name instead -- a client that read them as letters would believe a classic
//     reel can show a lowercase w.
//   * 'q' is not the letter q. The classic reel has no lowercase, so the char map borrowed that
//     byte for the DOUBLE-QUOTE flap. It folds to '"', which is what the flap actually shows.
//   * h u d s l t are the six SYMBOL flaps (flapsym.h): a reel that carries one is printed with
//     a heart, an arrow or a smiley, and that is what the character sets report. The letters
//     stay in the byte maps here -- they are what the reel holds -- and are rendered as their
//     symbols on the way out, so the set arithmetic never has to know about UTF-8.
// All three are exactly what the resolver does when it turns a request into a frame, so what
// capabilities promises is what the wall will do.
#ifndef SFGW_CAPSET_H
#define SFGW_CAPSET_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "flapsym.h"     // the colour and symbol codes, and what each symbol is called

// Fold one module's reel into the running union/common maps. A reel is a set of BYTES, so a
// 256-entry map is the whole of it: no sorting, no dedupe, no allocation.
//
// `first` must be true for the FIRST KNOWN reel only -- the intersection has to START from a
// real reel. Seeding `com` as all-true and intersecting from there would be wrong the moment a
// wall has one module: every byte no module has would survive as "common".
static inline void capFoldReel(const char* reel, bool* uni, bool* com, bool first) {
  bool here[256];
  memset(here, 0, sizeof(here));
  for (const char* c = reel; *c; c++) {
    uint8_t b = (uint8_t)*c;
    if (strchr(FLAP_COLOUR_CODES, (char)b)) continue;   // a colour flap, not a character
    if (b == 'q') b = '"';                              // the double-quote flap's borrowed byte
    here[b] = true;
  }
  for (int b = 0; b < 256; b++) {
    if (here[b]) uni[b] = true;
    if (first)        com[b] = here[b];
    else if (!here[b]) com[b] = false;                  // common = intersection
  }
}

// The symbols SOME reel carries, in table order, as indexes into FLAP_SYMBOLS. That is the
// union -- `pictographs` answers "is there a heart anywhere on this wall", the way `colors`
// does for the colour flaps. Returns how many were written.
static inline int capPictographs(const bool* uni, int* idxOut, int maxN) {
  int n = 0;
  for (int i = 0; i < FLAP_SYM_COUNT && n < maxN; i++)
    if (uni[(uint8_t)FLAP_SYMBOLS[i].code]) idxOut[n++] = i;
  return n;
}

// Should `spelling` carry the '"' -> 'q' entry? Only when some known reel holds the borrowed
// byte and none prints '"' itself. When reels disagree there is no single answer for the wall,
// so the entry is left out and a client that cares reads sets[].chars per module.
static inline bool capQuoteSpelling(bool anyBorrowedQ, bool anyPrintedQuote) {
  return anyBorrowedQ && !anyPrintedQuote;
}

// Compress a SORTED-ASCENDING id list into "0-44,50". Returns the length written. A run of one
// is written bare ("50"), never as "50-50".
static inline int capRangeList(const uint8_t* ids, int n, char* out, int outSize) {
  int len = 0;
  if (outSize > 0) out[0] = 0;
  int i = 0;
  while (i < n) {
    int lo = ids[i], hi = lo;
    while (i + 1 < n && (int)ids[i + 1] == hi + 1) { i++; hi = ids[i]; }
    i++;
    char part[24];
    int  pn = (lo == hi) ? snprintf(part, sizeof(part), "%s%d", len ? "," : "", lo)
                         : snprintf(part, sizeof(part), "%s%d-%d", len ? "," : "", lo, hi);
    if (pn < 0 || len + pn >= outSize) break;           // no room: stop cleanly, never overrun
    memcpy(out + len, part, pn);
    len += pn;
    out[len] = 0;
  }
  return len;
}

#endif // SFGW_CAPSET_H
