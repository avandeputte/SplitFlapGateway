// capset_test.cpp -- regression test for the set arithmetic behind GET /api/capabilities.
//
// Compiles the FIRMWARE'S OWN capset.h, not a copy of it:
//
//   g++ -std=c++17 -I src -o /tmp/capset_test tools/capset_test.cpp && /tmp/capset_test
//
// This logic cannot be exercised on real hardware without physically rebuilding the wall out of
// modules with different reels, which is precisely the case most likely to be wrong.

#include "capset.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void ok(bool cond, const char* what) {
  printf("  %s  %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) failures++;
}

// Render a byte map the way the endpoint streams it: reel order, printable bytes only.
static void mapToStr(const bool* m, char* out) {
  int n = 0;
  for (int b = 0x20; b < 256; b++) if (m[b]) out[n++] = (char)b;
  out[n] = 0;
}

static void eq(const bool* map, const char* want, const char* what) {
  char got[300];
  mapToStr(map, got);
  bool same = strcmp(got, want) == 0;
  printf("  %s  %s\n", same ? "PASS" : "FAIL", what);
  if (!same) { printf("         want '%s'\n         got  '%s'\n", want, got); failures++; }
}

static void ranges(const uint8_t* ids, int n, const char* want, const char* what) {
  char got[128];
  capRangeList(ids, n, got, sizeof(got));
  bool same = strcmp(got, want) == 0;
  printf("  %s  %s -> \"%s\"\n", same ? "PASS" : "FAIL", what, got);
  if (!same) { printf("         want \"%s\"\n", want); failures++; }
}

int main() {
  const char* CLASSIC = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw";

  printf("\nthe classic 64-flap reel\n");
  {
    bool uni[256] = {false}, com[256] = {false};
    capFoldReel(CLASSIC, uni, com, true);
    char got[300];
    mapToStr(uni, got);
    ok(strchr(got, 'q') == NULL, "'q' is NOT reported as a letter (it is the double-quote flap)");
    ok(strchr(got, '"') != NULL, "'\"' IS reported -- what that flap actually shows");
    ok(strchr(got, 'r') == NULL && strchr(got, 'w') == NULL,
       "the colour codes r..w are NOT reported as letters");
    ok(strchr(got, 'A') && strchr(got, 'Z') && strchr(got, '0') && strchr(got, '9'),
       "A-Z and 0-9 are reported");
    // 64 flaps, less the 7 colour codes. 'q' does not vanish -- it BECOMES '"', a byte the reel
    // does not otherwise carry, so it folds onto nothing and the count does not drop for it.
    ok(strlen(got) == 57, "57 characters: 64 flaps less the 7 colour codes");
    // union == common for a single reel, always.
    char c2[300];
    mapToStr(com, c2);
    ok(strcmp(got, c2) == 0, "one reel: common == union");
  }

  printf("\nthe mixed wall (module 1 has A-Z, module 2 has 0-9)\n");
  {
    bool uni[256] = {false}, com[256] = {false};
    capFoldReel(" ABCDEFGHIJKLMNOPQRSTUVWXYZ", uni, com, true);
    capFoldReel(" 0123456789",                 uni, com, false);
    eq(uni, " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", "union is A-Z AND 0-9");
    eq(com, " ", "common is the SPACE alone -- the wall cannot show \"HI42\" anywhere it likes");
  }

  printf("\nthe intersection must start from a real reel\n");
  {
    // Seeding `com` all-true and intersecting from there is the obvious implementation, and it
    // is wrong: on a one-module wall every byte no module has would survive as "common".
    bool uni[256] = {false}, com[256] = {false};
    capFoldReel(" AB", uni, com, true);
    eq(com, " AB", "a one-module wall: common is exactly that module's reel");
    ok(!com[(uint8_t)'Z'], "a byte NO module has is not 'common'");
  }

  printf("\na uniform wall stays uniform\n");
  {
    bool uni[256] = {false}, com[256] = {false};
    for (int i = 0; i < 45; i++) capFoldReel(CLASSIC, uni, com, i == 0);
    char u[300], c[300];
    mapToStr(uni, u); mapToStr(com, c);
    ok(strcmp(u, c) == 0, "45 identical reels: union == common");
  }

  printf("\nan empty reel intersects to nothing\n");
  {
    bool uni[256] = {false}, com[256] = {false};
    capFoldReel(" AB", uni, com, true);
    capFoldReel("",    uni, com, false);
    eq(uni, " AB", "union keeps what the other module has");
    eq(com, "",    "common is empty");
  }

  printf("\nrange compression\n");
  {
    uint8_t a[] = {0,1,2,3,4};                 ranges(a, 5, "0-4",        "a single run");
    uint8_t b[] = {0};                         ranges(b, 1, "0",          "one id is not '0-0'");
    uint8_t c[] = {0,1,2,5,6,9};               ranges(c, 6, "0-2,5-6,9",  "runs and a singleton");
    uint8_t d[] = {3,7,11};                    ranges(d, 3, "3,7,11",     "no runs at all");
    uint8_t e[] = {250,251,252,253,254};       ranges(e, 5, "250-254",    "the top of the id space");
    ranges(NULL, 0, "", "no modules at all");

    // Every id, one run. This is the shape a real wall actually has.
    uint8_t all[255];
    for (int i = 0; i < 255; i++) all[i] = (uint8_t)i;
    ranges(all, 255, "0-254", "all 255 ids collapse to one range");

    // A buffer too small must stop cleanly, not overrun.
    char tiny[8];
    uint8_t f[] = {1,3,5,7,9,11,13,15,17,19};
    int n = capRangeList(f, 10, tiny, sizeof(tiny));
    ok(n < (int)sizeof(tiny) && tiny[n] == 0, "a short buffer truncates cleanly and stays terminated");
  }

  printf("\n%s\n\n", failures ? "FAILURES" : "all passed");
  return failures ? 1 : 0;
}
