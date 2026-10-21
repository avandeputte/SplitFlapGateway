// symbol_test.cpp -- regression test for the symbol flaps: decoding them, resolving them against
// a reel, and keeping their internal codes off the bus.
//
// Compiles the FIRMWARE'S OWN table and rules, not a copy of them:
//
//   g++ -std=c++17 -I src -o /tmp/symbol_test tools/symbol_test.cpp src/charset.cpp && /tmp/symbol_test
//
// What makes this worth a native test: the interesting cases are a wall printed with symbols on
// some modules and not others, and you cannot produce that without physically reprinting reels.

#include "flapsym.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void ok(bool cond, const char* what) {
  printf("  %s  %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) failures++;
}

static void eqs(const char* got, const char* want, const char* what) {
  bool same = strcmp(got, want) == 0;
  printf("  %s  %s\n", same ? "PASS" : "FAIL", what);
  if (!same) { printf("         want '%s'\n         got  '%s'\n", want, got); failures++; }
}

// One decoded byte, as utf8ToFlap gives it, with whether everything mapped.
static void decodes(const char* in, const char* wantBytes, bool wantAll, const char* what) {
  char got[64];
  bool all = true;
  utf8ToFlap(in, got, sizeof(got), &all);
  bool same = strcmp(got, wantBytes) == 0 && all == wantAll;
  printf("  %s  %s\n", same ? "PASS" : "FAIL", what);
  if (!same) {
    printf("         want %u byte(s) [", (unsigned)strlen(wantBytes));
    for (const char* p = wantBytes; *p; p++) printf(" %02X", (uint8_t)*p);
    printf(" ] allMapped=%d\n         got  %u byte(s) [", wantAll, (unsigned)strlen(got));
    for (const char* p = got; *p; p++) printf(" %02X", (uint8_t)*p);
    printf(" ] allMapped=%d\n", all);
    failures++;
  }
}

static const char HEART[]  = "♥";
static const char UP[]     = "↑";
static const char DOWN[]   = "↓";
static const char SMILEY[] = "☺";
static const char LEFT[]   = "←";
static const char RIGHT[]  = "→";

int main() {
  // The classic reel, and the same reel with three punctuation flaps reprinted as ♥ ↑ ↓.
  const char* CLASSIC = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw";
  const char* SYMREEL = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789hud$&()-+=;q:%'.,/?*roygbpw";
  //                                                          ^^^ were ! @ #
  const char* QUOTEREEL = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;\":%'.,/?*roygbpw";

  printf("\ndecoding symbols from UTF-8\n");
  {
    decodes(HEART,  "\x01", true, "♥ decodes to the heart's internal code");
    decodes(UP,     "\x02", true, "↑ decodes to the up code");
    decodes(DOWN,   "\x03", true, "↓ decodes to the down code");
    decodes(SMILEY, "\x04", true, "☺ decodes to the smiley code");
    decodes(LEFT,   "\x05", true, "← decodes to the left code");
    decodes(RIGHT,  "\x06", true, "→ decodes to the right code");

    decodes("❤",  "\x01", true, "the emoji heart ❤ is the same flap");
    decodes("\U0001F642", "\x04", true, "🙂 is the smiley");
    decodes("\U0001F60A", "\x04", true, "😊 is the smiley");
    decodes("\U0001F600", "\x04", true, "😀 is the smiley");
    decodes("⬆", "\x02", true, "⬆ is the up arrow");
    decodes("⬇", "\x03", true, "⬇ is the down arrow");
    decodes("⬅", "\x05", true, "⬅ is the left arrow");
    decodes("➡", "\x06", true, "➡ is the right arrow");
  }

  printf("\nvariation selectors belong to the symbol before them\n");
  {
    decodes("❤️", "\x01", true, "❤️ (heart + U+FE0F) is ONE flap, fully mapped");
    decodes("♥︎", "\x01", true, "♥︎ (heart + U+FE0E) is one flap too");
    decodes("⬆️", "\x02", true, "⬆️ is one flap");
    decodes("A❤️B", "A\x01" "B", true, "a heart inside text stays ONE module wide");
    // A lone selector is not a symbol, so it is still an unmappable code point.
    decodes("️", "", false, "a variation selector on its own is still dropped");
  }

  printf("\neverything else decodes exactly as before\n");
  {
    decodes("HELLO", "HELLO", true, "plain ASCII");
    decodes("hello", "hello", true, "lowercase is not folded HERE (the resolver does that)");
    decodes("€", "\x80", true, "the euro sign is still CP1252 0x80");
    decodes("é", "\xE9", true, "é is still 0xE9");
    decodes("…", "\x85", true, "the ellipsis is still 0x85");
    decodes("\U0001F680", "", false, "an unknown emoji is still dropped, allMapped false");
    decodes("A\U0001F680B", "AB", false, "...and the rest of the text still maps");
    ok(!isFlapByte(0x01) && !isFlapByte(0x06), "internal codes are not flap bytes");
  }

  printf("\nresolving a symbol against one module's reel\n");
  {
    ok(flapResolveFor(0x01, SYMREEL) == 'h', "a reel WITH the heart gets the letter");
    ok(flapResolveFor(0x02, SYMREEL) == 'u', "...and the up arrow");
    ok(flapResolveFor(0x03, SYMREEL) == 'd', "...and the down arrow");
    ok(flapResolveFor(0x01, CLASSIC) == '*', "a reel without it gets the stand-in it DOES have");
    ok(flapResolveFor(0x04, CLASSIC) == ':', "the smiley stands in as ':' on the classic reel");
    ok(flapResolveFor(0x03, CLASSIC) == 'V', "the down arrow stands in as 'V'");
    ok(flapResolveFor(0x02, CLASSIC) == ' ',
       "no arrow and no '^' on the classic reel: a blank, so nothing shifts");
    ok(flapResolveFor(0x05, CLASSIC) == ' ', "same for ←");
    ok(flapResolveFor(0x06, CLASSIC) == ' ', "same for →");
    ok(flapResolveFor(0x01, NULL) == 'h',  "an UNKNOWN reel gets the letter, as any character does");
    ok(flapResolveFor(0x01, "")   == 'h',  "an empty reel is the same as unknown");
  }

  printf("\ntyped letters are never symbols\n");
  {
    ok(flapResolveFor('h', SYMREEL) == 'H', "a typed 'h' is still an H, even on a heart reel");
    ok(flapResolveFor('u', SYMREEL) == 'U', "a typed 'u' is still a U");
    ok(flapResolveFor('d', CLASSIC) == 'D', "a typed 'd' is still a D");
    ok(flapResolveFor('s', CLASSIC) == 'S' && flapResolveFor('t', CLASSIC) == 'T' &&
       flapResolveFor('l', CLASSIC) == 'L', "s, t and l fold too");
    ok(flapResolveFor('A', CLASSIC) == 'A', "capitals pass through");
  }

  printf("\nthe colour flaps are untouched\n");
  {
    const char* colours = FLAP_COLOUR_CODES;
    bool allKept = true;
    for (const char* c = colours; *c; c++)
      if (flapResolveFor((uint8_t)*c, CLASSIC) != (uint8_t)*c) allKept = false;
    ok(allKept, "r o y g b p w go out as themselves, on any reel");
    ok(flapResolveFor('r', NULL) == 'r', "...with an unknown reel too");
  }

  printf("\nthe double-quote flap, both ways round\n");
  {
    ok(flapResolveFor('"', CLASSIC)   == 'q',  "a reel that borrows 'q' for it gets 'q'");
    ok(flapResolveFor('"', QUOTEREEL) == '"',  "a reel PRINTED with '\"' gets the byte itself");
    ok(flapResolveFor('"', NULL)      == 'q',  "an unknown reel gets 'q', as before");
    ok(flapResolveFor('q', CLASSIC)   == 'Q',  "a typed 'q' is still the letter Q");
  }

  printf("\nnothing showable\n");
  {
    ok(flapResolveFor(0x00, CLASSIC) == 0, "the end-of-set sentinel resolves to nothing");
    ok(flapResolveFor(0xFF, CLASSIC) == 0, "the unused-flap sentinel resolves to nothing");
    ok(flapResolveFor(0x1F, CLASSIC) == 0, "a control byte resolves to nothing");
  }

  printf("\ninternal codes never reach the bus\n");
  {
    const uint8_t bad1[] = { 'm', '5', '-', 0x01 };
    const uint8_t bad2[] = { 'm', '*', '-', 0x06 };
    const uint8_t bad3[] = { 'm', '1', '2', '-', 0x04 };
    const uint8_t good1[] = { 'm', '5', '-', 'h' };
    const uint8_t good2[] = { 'm', '5', '-', 'A' };
    const uint8_t good3[] = { 'm', '5', 'h' };
    ok(flapFrameShowsInternal(bad1, sizeof(bad1)),  "m5- + an internal code is refused");
    ok(flapFrameShowsInternal(bad2, sizeof(bad2)),  "a broadcast of one is refused");
    ok(flapFrameShowsInternal(bad3, sizeof(bad3)),  "a two-digit id is parsed too");
    ok(!flapFrameShowsInternal(good1, sizeof(good1)), "the resolved letter passes");
    ok(!flapFrameShowsInternal(good2, sizeof(good2)), "an ordinary character passes");
    ok(!flapFrameShowsInternal(good3, sizeof(good3)), "a home frame passes");
  }

  printf("\nconfiguring a reel stores the letters\n");
  {
    char reel[80];
    bool all = true;
    utf8ToFlap(" AB♥↑↓", reel, sizeof(reel), &all);
    flapSymToLetters(reel);
    eqs(reel, " ABhud", "a reel pasted with ♥ ↑ ↓ is stored as h u d");
    ok(all, "...and nothing was reported unmappable");
    ok(strlen(reel) == 6, "six flaps, not six times three UTF-8 bytes");

    char mixed[80];
    utf8ToFlap("h♥", mixed, sizeof(mixed), &all);
    flapSymToLetters(mixed);
    eqs(mixed, "hh", "a typed 'h' and a ♥ both store as 'h' -- the reel holds bytes, not intent");
  }

  printf("\nreporting what a module shows\n");
  {
    char out[8]; size_t n;
    n = flapShownToUtf8('h', out); out[n] = 0;  eqs(out, HEART, "'h' shows as ♥");
    n = flapShownToUtf8('u', out); out[n] = 0;  eqs(out, UP,    "'u' shows as ↑");
    n = flapShownToUtf8('t', out); out[n] = 0;  eqs(out, RIGHT, "'t' shows as →");
    n = flapShownToUtf8('q', out); out[n] = 0;  eqs(out, "\"",  "'q' shows as a double quote");
    n = flapShownToUtf8('A', out); out[n] = 0;  eqs(out, "A",   "a letter shows as itself");
    n = flapShownToUtf8('r', out); out[n] = 0;  eqs(out, "r",   "a colour code stays a letter");
    n = flapShownToUtf8(0x80, out); out[n] = 0; eqs(out, "€", "0x80 still shows as the euro sign");

    char js[8];
    flapShownToJson('q', js, sizeof(js));
    eqs(js, "\\\"", "the double-quote flap is escaped for JSON");
    flapShownToJson('h', js, sizeof(js));
    eqs(js, HEART, "a symbol needs no escaping");
  }

  printf("\n%s\n\n", failures ? "FAILURES" : "all passed");
  return failures ? 1 : 0;
}
