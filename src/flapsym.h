// flapsym.h -- the symbol flaps, and the one table that defines them.
//
// People print symbols on some flaps: a heart, arrows, a smiley. The bus carries exactly ONE
// byte per flap (see charset.h) and a module is addressed BY that byte, and these symbols have
// no Windows-1252 byte. So each one gets a reserved lowercase letter on the wire, exactly as the
// seven colour flaps are r/o/y/g/b/p/w and the classic reel's double-quote flap is 'q'. Wire
// protocol, not repertoire -- which is why clients never see the letters: they send and receive
// the real symbol (♥) and the gateway translates at the edges.
//
// Three rules keep that from becoming a trap:
//   * A TYPED letter is never a symbol. 'h' still folds to 'H' on every text path, because that
//     is what every existing client relies on. Only ♥ (or one of its aliases) reaches the heart.
//   * A symbol only exists where it is PRINTED. A module's reel is the authority: without the
//     letter the gateway sends the stand-in that reel does have, or a blank, so text never
//     shifts and no module hunts for a flap it hasn't got.
//   * The letters never leave the gateway as letters. Capabilities reports the symbols, so a
//     client that read `h` as a lowercase h would believe a reel can show one.
//
// 'm' and 'v' are deliberately NOT assignable: a checksummed frame restarts at any 'm'
// (tasks.cpp), so a reel carrying one would cut its own module's reply short, and 'v' was the
// companion's old stand-in for a down arrow. Still free for later: a c e f i j k n x z.
//
// This header is deliberately free of Arduino, like charset.h and capset.h, so the firmware and
// the native tests (tools/symbol_test.cpp, tools/capset_test.cpp) compile the SAME table and the
// SAME resolution rules instead of two copies of a guess. common.h includes it where the colour
// codes used to be defined, so `#include "gateway.h"` still gets you everything.

#ifndef SFGW_FLAPSYM_H
#define SFGW_FLAPSYM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "charset.h"      // isFlapByte -- the shared idea of "a byte the wire may carry"

// The seven colour flaps, conveyed as lowercase letters by the module protocol.
#define FLAP_COLOUR_CODES  "roygbpw"
// The six symbol flaps, same idea. Table order, and the order they are reported in.
#define FLAP_SYMBOL_CODES  "hudslt"

#define FLAP_SYM_COUNT     6
// Internal codes utf8ToFlap() produces for a decoded symbol, in table order. They exist only
// between decoding a request and resolving it against a module's reel: isFlapByte() rejects
// 0x00-0x1F, so an unresolved one can never pass as a flap, and rs485Send() refuses a frame
// carrying one outright (belt and braces -- a leak would make a module hunt for flap 1).
#define FLAP_SYM_FIRST     0x01
#define FLAP_SYM_LAST      (FLAP_SYM_FIRST + FLAP_SYM_COUNT - 1)

struct FlapSymbol {
  char        code;       // the reserved lowercase letter carried on the wire
  uint32_t    cp;         // the symbol itself, as a Unicode code point
  const char* name;       // the companion's pictograph name, verbatim (renderer.py PICTOGRAPHS)
  char        standIn;    // what a reel without this symbol shows instead, if it has it
  uint32_t    alias[4];   // other code points that mean the same symbol, 0-terminated
};

// The table. Aliases are the shapes a phone keyboard or a copy-paste actually produces: the
// emoji heart ❤ and the emoji arrows ⬆⬇⬅➡ are different code points from the dingbat ones,
// and a smiley arrives as any of three faces.
static const FlapSymbol FLAP_SYMBOLS[FLAP_SYM_COUNT] = {
  { 'h', 0x2665, "heart",  '*', { 0x2764,  0, 0, 0 } },
  { 'u', 0x2191, "up",     '^', { 0x2B06,  0, 0, 0 } },
  { 'd', 0x2193, "down",   'V', { 0x2B07,  0, 0, 0 } },
  { 's', 0x263A, "smiley", ':', { 0x1F642, 0x1F60A, 0x1F600, 0 } },
  { 'l', 0x2190, "left",   '<', { 0x2B05,  0, 0, 0 } },
  { 't', 0x2192, "right",  '>', { 0x27A1,  0, 0, 0 } },
};

// U+FE0E / U+FE0F: the text/emoji variation selectors. They follow a symbol and change nothing
// about which flap is meant, so they are swallowed with it -- otherwise "❤️" would decode as a
// heart plus one dropped code point, and a caller would be told its text was unrepresentable.
static inline bool flapSymIsVariationSelector(uint32_t cp) {
  return cp == 0xFE0E || cp == 0xFE0F;
}

// Table index for a code point (the symbol's own, or any of its aliases), or -1.
static inline int flapSymFromCp(uint32_t cp) {
  for (int i = 0; i < FLAP_SYM_COUNT; i++) {
    if (FLAP_SYMBOLS[i].cp == cp) return i;
    for (int a = 0; a < 4 && FLAP_SYMBOLS[i].alias[a]; a++)
      if (FLAP_SYMBOLS[i].alias[a] == cp) return i;
  }
  return -1;
}

// Table index for a wire letter ('h'...), or -1. Case-sensitive on purpose: 'H' is the letter.
static inline int flapSymFromLetter(char c) {
  for (int i = 0; i < FLAP_SYM_COUNT; i++) if (FLAP_SYMBOLS[i].code == c) return i;
  return -1;
}

static inline bool flapSymIsInternal(uint8_t b) {
  return b >= FLAP_SYM_FIRST && b <= FLAP_SYM_LAST;
}

// Table index for an internal code, or -1.
static inline int flapSymFromInternal(uint8_t b) {
  return flapSymIsInternal(b) ? (int)(b - FLAP_SYM_FIRST) : -1;
}

static inline uint8_t flapSymInternal(int idx) {
  return (uint8_t)(FLAP_SYM_FIRST + idx);
}

// Resolve one requested byte to the byte that actually goes on the wire for a given module, or
// 0 for "nothing showable". `reel` is that module's flap set, or NULL/"" when the gateway does
// not know it yet. The rules, all of them the reel's and not this function's:
//
//   * A SYMBOL (an internal code from utf8ToFlap) becomes its letter when the reel carries it,
//     the stand-in when the reel carries that instead, and a blank otherwise. An unknown reel
//     gets the letter, which is what every other character does today.
//   * ASCII lowercase folds to uppercase -- the classic reel is printed in capitals -- EXCEPT
//     the seven colour codes, which the protocol conveys as lowercase letters. Folding a typed
//     'h' to 'H' is deliberate: typed letters are never symbols.
//   * '"' reaches the double-quote flap. A reel that prints '"' directly gets the byte itself;
//     every other reel gets 'q', the byte the classic char map borrowed for that flap. Sending
//     'q' to a reel that prints '"' was an old bug: the module has no 'q' and re-homes.
static inline uint8_t flapResolveFor(uint8_t b, const char* reel) {
  const bool known = (reel && reel[0]);

  int si = flapSymFromInternal(b);
  if (si >= 0) {
    const char letter = FLAP_SYMBOLS[si].code;
    if (!known)                  return (uint8_t)letter;
    if (strchr(reel, letter))    return (uint8_t)letter;
    const char alt = FLAP_SYMBOLS[si].standIn;
    if (alt && strchr(reel, alt)) return (uint8_t)alt;
    return (uint8_t)' ';
  }

  if (b >= 'a' && b <= 'z' && !strchr(FLAP_COLOUR_CODES, (char)b)) b = (uint8_t)(b - 'a' + 'A');
  if (b == '"') return (known && strchr(reel, '"')) ? (uint8_t)'"' : (uint8_t)'q';
  return isFlapByte(b) ? b : 0;
}

// True if a '-' (show character) frame carries an unresolved internal code. rs485Send() refuses
// such a frame: every path either resolves the code against a reel or converts it to a letter,
// and a byte of 0x01 on the wire would be a bug worth stopping at the door rather than a flap.
static inline bool flapFrameShowsInternal(const uint8_t* data, size_t len) {
  if (len < 4 || data[0] != 'm') return false;
  size_t i = 1;
  if (data[i] == '*') { i++; if (i < len && data[i] == '*') i++; }
  else { size_t d = i; while (i < len && data[i] >= '0' && data[i] <= '9') i++; if (i == d) return false; }
  if (i >= len || data[i] != '-') return false;
  return (i + 1 < len) && flapSymIsInternal(data[i + 1]);
}

// Rewrite internal codes to their wire letters, in place. Configuration paths use this: a reel
// is STORED as letters (that is what the module keeps and reports), while a display path
// resolves the same code against the reel instead.
static inline void flapSymToLetters(char* s) {
  for (; s && *s; s++) {
    int si = flapSymFromInternal((uint8_t)*s);
    if (si >= 0) *s = FLAP_SYMBOLS[si].code;
  }
}

#endif // SFGW_FLAPSYM_H
