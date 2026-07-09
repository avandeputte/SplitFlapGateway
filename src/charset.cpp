// charset.cpp -- UTF-8 <-> flap-byte transcoding. See charset.h.
//
// Flap-byte encoding: Windows-1252 (CP1252). It is identical to Latin-1 for
// 0x00-0x7F and 0xA0-0xFF, and defines printable characters (euro, smart quotes,
// dashes, ellipsis, trademark, a few letters) in 0x80-0x9F where Latin-1 had C1
// controls. Five of those slots (0x81/0x8D/0x8F/0x90/0x9D) are undefined.

#include "charset.h"

// Unicode code point for each CP1252 byte 0x80..0x9F (0 = undefined slot).
static const uint16_t CP1252_HIGH[32] = {
  0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, // 80-87
  0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000, // 88-8F
  0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 90-97
  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178  // 98-9F
};

// Map a CP1252 byte to its Unicode code point (0x00-0x7F and 0xA0-0xFF are
// identity; 0x80-0x9F use the table, which may be 0 for an undefined slot).
static uint32_t cp1252ToUnicode(uint8_t b) {
  if (b >= 0x80 && b <= 0x9F) return CP1252_HIGH[b - 0x80];
  return b;
}

// Map a Unicode code point to its CP1252 byte, or -1 if unrepresentable.
static int unicodeToCp1252(uint32_t cp) {
  if (cp >= 0x20 && cp <= 0x7E) return (int)cp;          // ASCII printable
  if (cp >= 0xA0 && cp <= 0xFF) return (int)cp;          // Latin-1 high range
  for (int i = 0; i < 32; i++)                           // the 0x80-0x9F extras
    if (CP1252_HIGH[i] && CP1252_HIGH[i] == cp) return 0x80 + i;
  return -1;
}

bool isFlapByte(uint8_t b) {
  if (b >= 0x20 && b <= 0x7E) return true;               // ASCII printable
  if (b >= 0x80 && b <= 0x9F) return CP1252_HIGH[b - 0x80] != 0;  // defined extras
  if (b == 0xA0 || b == 0xAD) return false;              // NBSP / soft hyphen
  return b >= 0xA1 && b <= 0xFE;                         // Latin-1 letters (not 0xFF)
}

size_t utf8ToFlap(const char* in, char* out, size_t outSize, bool* allMapped) {
  size_t oi = 0;
  bool   mapped = true;
  const uint8_t* p = (const uint8_t*)in;
  if (outSize == 0) { if (allMapped) *allMapped = false; return 0; }

  while (*p) {
    uint8_t  b = *p;
    uint32_t cp;
    int      n;
    if      (b < 0x80)          { cp = b;        n = 1; }
    else if ((b & 0xE0) == 0xC0){ cp = b & 0x1F; n = 2; }
    else if ((b & 0xF0) == 0xE0){ cp = b & 0x0F; n = 3; }
    else if ((b & 0xF8) == 0xF0){ cp = b & 0x07; n = 4; }
    else { p++; mapped = false; continue; }              // invalid lead byte

    bool ok = true;
    for (int k = 1; k < n; k++) {
      if ((p[k] & 0xC0) != 0x80) { ok = false; break; }  // not a continuation
      cp = (cp << 6) | (p[k] & 0x3F);
    }
    if (!ok) { p++; mapped = false; continue; }          // truncated sequence
    p += n;

    int fb = unicodeToCp1252(cp);
    if (fb < 0 || !isFlapByte((uint8_t)fb)) { mapped = false; continue; }
    if (oi + 1 >= outSize)             { mapped = false; break; }   // no room left
    out[oi++] = (char)(uint8_t)fb;
  }
  out[oi] = '\0';
  if (allMapped) *allMapped = mapped;
  return oi;
}

size_t flapByteToUtf8(uint8_t b, char* out) {
  uint32_t cp = cp1252ToUnicode(b);
  if (cp == 0) cp = b;                 // undefined slot: emit the raw byte's value
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  out[0] = (char)(0xE0 | (cp >> 12));
  out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[2] = (char)(0x80 | (cp & 0x3F));
  return 3;
}

size_t flapToJsonUtf8(const char* in, size_t inLen, char* out, size_t outSize, char junk) {
  size_t oi = 0;
  for (size_t i = 0; i < inLen; i++) {
    uint8_t b = (uint8_t)in[i];
    if (b == '\n' || b == '\r') continue;        // strip line breaks
    if (b == '"' || b == '\\') {                 // JSON-escape
      if (oi + 2 >= outSize) break;
      out[oi++] = '\\';
      out[oi++] = (char)b;
      continue;
    }
    if (!isFlapByte(b)) {                         // control / sentinel / undefined slot
      if (junk) { if (oi + 1 >= outSize) break; out[oi++] = junk; }
      continue;                                   // junk == 0: drop
    }
    char enc[4];                                  // representable glyph -> UTF-8
    size_t n = flapByteToUtf8(b, enc);
    if (oi + n + 1 > outSize) break;
    for (size_t k = 0; k < n; k++) out[oi++] = enc[k];
  }
  out[oi] = '\0';
  return oi;
}
