#!/usr/bin/env python3
"""Generate src/web_ui.h from the UI sources.

    ui/index.html      the dashboard (HTML + CSS + JS) -- the source of truth
    ui/strings/*.json  one translation dictionary per language, keyed by the
                       ENGLISH text ({"Save": "Enregistrer", ...})
        |
        v
    src/web_ui.h       PAGE_HTML + every dictionary, gzipped, in flash

One firmware image ships every language; the browser fetches only the one it
needs from GET /lang/<code>. English needs no dictionary at all -- it is the
text already in the page, which is also the per-key fallback, so a missing or
partial translation degrades to English instead of breaking.

Dictionaries are stored gzipped (they compress ~4x) and served with
Content-Encoding: gzip, which is honest here: the gzip is a transfer encoding
of JSON the browser decompresses itself. (Contrast the companion settings blob,
where the gzip IS the payload and that header would be wrong.)

Run:  python3 tools/build_ui.py          (writes src/web_ui.h)
      python3 tools/build_ui.py --check  (verify src/web_ui.h is up to date)
"""
import gzip
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
UI = ROOT / "ui"
OUT = ROOT / "src" / "web_ui.h"

BASE_LANG = "en"          # the text already in the page; never shipped as a dict

HEADER = """#ifndef SFGW_WEB_UI_H
#define SFGW_WEB_UI_H

// GENERATED FILE -- do not edit by hand.
// Source: ui/index.html + ui/strings/*.json   Regenerate: python3 tools/build_ui.py
//
// Dashboard page markup (HTML + CSS + JS), served verbatim by handleRoot().
// Streamed byte-for-byte; the page pulls all runtime values (config, modules,
// bus) via the REST API. Included only by web.cpp, so the arrays have one home.
//
// UI_LANGS[] holds one gzipped JSON dictionary per language, served by
// handleLang() at GET /lang/<code>. English is not in the table: it is the text
// already in the page, and the per-key fallback for every other language.

static const char PAGE_HTML[] = R"=====("""

FOOTER = """
#endif // SFGW_WEB_UI_H
"""


def c_bytes(name: str, blob: bytes) -> str:
    """Emit a gzipped dictionary as a PROGMEM byte array."""
    rows = []
    for i in range(0, len(blob), 16):
        rows.append("  " + " ".join(f"0x{b:02x}," for b in blob[i:i + 16]))
    return f"static const uint8_t {name}[] PROGMEM = {{\n" + "\n".join(rows) + "\n};\n"


def load_dicts() -> list[tuple[str, str, bytes]]:
    """Return [(code, native_name, gzipped_json)] for every non-English dictionary."""
    out = []
    if not (UI / "strings").is_dir():
        return out
    for f in sorted((UI / "strings").glob("*.json")):
        code = f.stem
        d = json.loads(f.read_text(encoding="utf-8"))
        name = d.pop("$name", code)          # native language name, for the picker
        if code == BASE_LANG:
            continue
        # Drop entries that are untranslated (value == key): the runtime already
        # falls back to English, so shipping them is pure flash for no effect.
        d = {k: v for k, v in d.items() if v and v != k}
        if not d:
            continue
        raw = json.dumps(d, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        gz = gzip.compress(raw, 9, mtime=0)   # mtime=0 -> byte-stable output
        out.append((code, name, gz))
    return out


def build() -> str:
    page = (UI / "index.html").read_text(encoding="utf-8")
    if ')====="' in page:
        sys.exit("ERROR: ui/index.html contains the raw-string terminator )=====\"")

    dicts = load_dicts()
    parts = [HEADER, page, ')====="', ";\n"]

    if dicts:
        parts.append("\n")
        for code, _name, gz in dicts:
            parts.append(c_bytes(f"LANG_{code.replace('-', '_').upper()}", gz))
        parts.append("\nstruct UiLang { const char* code; const char* name;"
                     " const uint8_t* gz; size_t len; };\n")
        parts.append("static const UiLang UI_LANGS[] = {\n")
        for code, name, gz in dicts:
            sym = f"LANG_{code.replace('-', '_').upper()}"
            parts.append(f'  {{ "{code}", "{name}", {sym}, sizeof({sym}) }},\n')
        parts.append("};\n")
        parts.append(f"static const size_t UI_LANG_COUNT = {len(dicts)};\n")
    else:
        # No dictionaries yet: still declare the table so web.cpp compiles.
        parts.append("\nstruct UiLang { const char* code; const char* name;"
                     " const uint8_t* gz; size_t len; };\n")
        parts.append("static const UiLang UI_LANGS[] = {};\n")
        parts.append("static const size_t UI_LANG_COUNT = 0;\n")

    parts.append(FOOTER)
    return "".join(parts)


def main() -> None:
    text = build()
    if "--check" in sys.argv:
        cur = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if cur != text:
            sys.exit("src/web_ui.h is STALE -- run: python3 tools/build_ui.py")
        print("src/web_ui.h is up to date.")
        return

    OUT.write_text(text, encoding="utf-8")
    dicts = load_dicts()
    page_n = len((UI / "index.html").read_bytes())
    gz_n = sum(len(g) for _, _, g in dicts)
    print(f"page        {page_n:>8,} B")
    for code, name, gz in dicts:
        print(f"  {code:<6} {len(gz):>6,} B gz   {name}")
    print(f"dicts       {gz_n:>8,} B  ({len(dicts)} languages)")
    print(f"web_ui.h    {len(text.encode()):>8,} B")


if __name__ == "__main__":
    main()
