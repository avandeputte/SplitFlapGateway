// charset.h -- UTF-8 <-> flap-byte transcoding for the configurable flap set.
//
// The split-flap protocol carries exactly ONE byte per displayed character (the
// `-<char>` command and the configurable flap set are byte arrays in the module
// firmware). Browsers and JSON, however, speak UTF-8, where a euro sign is 3
// bytes and accented letters are 2. To let people use euro signs and accented
// characters on the flaps, the gateway bridges the two: it maps each UTF-8 code
// point to a single flap byte on the way to the bus, and maps each stored flap
// byte back to UTF-8 on the way to the browser/MQTT.
//
// The single-byte flap encoding is **Windows-1252 (CP1252)**: a superset of
// Latin-1 that puts the euro sign at 0x80 and adds smart quotes, dashes, the
// ellipsis, the trademark sign, etc. in 0x80-0x9F, while keeping every
// Western-European accented letter in 0xA0-0xFF. (CP1252 is the de-facto Windows
// default, so text pasted from common tools maps cleanly.) The encoding is an
// implementation detail of charset.cpp -- callers use the neutral helpers below,
// so swapping the code page later touches only that one file.
//
// Reserved bytes that are never produced for a flap: 0x00 (firmware end-of-set),
// 0xFF (firmware "unused flap" sentinel, = y-diaeresis), 0xA0 (NBSP), 0xAD (soft
// hyphen), the five CP1252 undefined slots (0x81/0x8D/0x8F/0x90/0x9D), and the
// C0/C1 control ranges.

#ifndef SFGW_CHARSET_H
#define SFGW_CHARSET_H

#include <stddef.h>
#include <stdint.h>

// Transcode a UTF-8 C-string to single flap bytes (Windows-1252). Only safe
// printable glyphs are produced (see the reserved-bytes note above). Control
// characters and any code point with no flap-byte representation are dropped.
// `out` is always NUL-terminated. Returns the number of bytes written. If
// `allMapped` is non-NULL it is set false when any input code point was dropped
// (unrepresentable, invalid, or buffer full).
//
// The SYMBOL flaps (flapsym.h) are the one exception to "one byte per glyph in
// Windows-1252": a heart has no CP1252 byte, so it decodes to an INTERNAL code
// (0x01-0x06) that the caller must resolve against a module's reel -- or convert
// to its wire letter, for a reel being configured. Internal codes are still one
// byte per glyph, so lengths and module counts are unaffected, and isFlapByte()
// rejects them so an unresolved one can never be mistaken for a flap. A
// variation selector (U+FE0E/U+FE0F) right after a symbol is swallowed with it.
size_t utf8ToFlap(const char* in, char* out, size_t outSize, bool* allMapped = nullptr);

// True if `b` is a byte the flap protocol may carry: a printable glyph in the
// flap encoding (Windows-1252) that is not a control, an undefined slot,
// NBSP/soft-hyphen, or a firmware sentinel (0x00 end-of-set / 0xFF unused-flap).
// Use this instead of hand-coded byte ranges so the encoding lives in one place.
bool isFlapByte(uint8_t b);

// Append the UTF-8 encoding of one flap byte to `out` (1-3 bytes, NOT
// NUL-terminated); returns the number of bytes written. `out` must have room for
// at least 3 bytes.
size_t flapByteToUtf8(uint8_t b, char* out);

// What a flap byte SHOWS, as UTF-8 (1-3 bytes, NOT NUL-terminated): the symbol
// for a symbol letter, '"' for the double-quote flap's 'q', and otherwise
// exactly what flapByteToUtf8() gives. This is for reporting what a module is
// displaying -- /api/display/state, /api/flap/modules, the MQTT display string.
// Everything that reports a REEL (sets[].chars, dumps, the bus monitor) must
// keep using flapByteToUtf8(): a reel is stored and addressed as the raw bytes.
// Colour codes stay letters here, as on the Matrix Gateway. `out` needs 4 bytes.
size_t flapShownToUtf8(uint8_t b, char* out);

// flapShownToUtf8() with JSON escaping: '"' and '\\' come back backslash-escaped,
// so the result can go straight inside a JSON string. NUL-terminated. `out`
// needs 6 bytes. Returns the length written (excluding the NUL).
size_t flapShownToJson(uint8_t b, char* out, size_t outSize);

// Transcode `inLen` flap bytes into a JSON-string-safe UTF-8 sequence in `out`:
//   * `"` and `\` are backslash-escaped
//   * line breaks (`\n`/`\r`) are stripped (structural, not content)
//   * representable flap glyphs (see isFlapByte) become their UTF-8 encoding
//   * every other byte -- control, sentinel, undefined CP1252 slot -- becomes
//     `junk`, or is dropped when `junk` is 0
// `out` is NUL-terminated and never split mid-glyph if it fills: output is
// capped at a glyph/escape boundary. Returns bytes written (excluding the NUL).
// Size `out` for up to 3 UTF-8 bytes (or 2 for an escape) per input byte plus a
// terminator. `junk` defaults to 0 (drop) to preserve existing call sites.
size_t flapToJsonUtf8(const char* in, size_t inLen, char* out, size_t outSize, char junk = 0);

#endif // SFGW_CHARSET_H
