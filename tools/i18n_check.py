#!/usr/bin/env python3
"""Validate the UI translation dictionaries against ui/strings/en.json.

Catches the failure modes that are otherwise SILENT -- a mistyped key simply never
matches at runtime, so the string quietly stays English and nobody notices:

  * a key that is not in en.json (typo, or a stale key after the UI text changed)
  * the product name dropped from a translation ("Split-Flap" is a name, not a word)
  * a value that smuggles in markup or a quote that would break the page
  * a character from outside the repertoire -- see ALLOWED_EXTRA below
  * an entry that just restates the English (dead weight in flash: the runtime already
    falls back per key)

Coverage is reported, not enforced: a partial dictionary is a supported, deliberate
state -- every missing key falls back to English.

Run: python3 tools/i18n_check.py        (exit 1 on any error)
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
STR = ROOT / "ui" / "strings"
BRAND = re.compile(r'split[\s-]?flap', re.I)

en = json.loads((STR / "en.json").read_text(encoding="utf-8"))
en_keys = {k for k in en if k != "$name"}

# The DASHBOARD is UTF-8 and may render anything; it is the flap MODULES that are limited
# to Windows-1252, and that limit is why the language *list* is what it is -- it is not a
# limit on the characters of the page. English itself uses "→" ("Open Firmware Updater →"),
# which is not in cp1252. So the rule is: Windows-1252, PLUS whatever the English source
# already uses. That still catches a stray CJK/Cyrillic/emoji character (a sign the
# translator wandered outside the intended repertoire) without failing an honest arrow.
ALLOWED_EXTRA = {c for k in en_keys for c in k}


def out_of_repertoire(v):
    bad = set()
    for c in str(v):
        if c in ALLOWED_EXTRA:
            continue
        try:
            c.encode("cp1252")
        except UnicodeEncodeError:
            bad.add(c)
    return bad

errors, rows = [], []
for f in sorted(STR.glob("*.json")):
    if f.stem == "en":
        continue
    try:
        d = json.loads(f.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        errors.append(f"{f.name}: INVALID JSON -- {e}")
        continue

    name = d.get("$name")
    if not name:
        errors.append(f"{f.name}: missing $name (the native language name)")

    live = {k: v for k, v in d.items() if k != "$name"}
    stale = [k for k in live if k not in en_keys]
    noop = [k for k, v in live.items() if v == k]
    brand = [k for k, v in live.items() if BRAND.search(k) and not BRAND.search(str(v))]
    markup = [k for k, v in live.items() if re.search(r'[<>]|&#|javascript:', str(v))]
    bad_enc = []
    for k, v in live.items():
        stray = out_of_repertoire(v)
        if stray:
            bad_enc.append((k, "".join(sorted(stray))))

    for k in stale:
        errors.append(f"{f.stem}: key not in en.json (typo/stale): {k[:50]!r}")
    for k in brand:
        errors.append(f"{f.stem}: product name lost: {k[:50]!r}")
    for k in markup:
        errors.append(f"{f.stem}: value contains markup: {k[:50]!r}")
    for k, stray in bad_enc:
        errors.append(f"{f.stem}: value uses {stray!r}, outside the repertoire: {k[:44]!r}")

    good = len(live) - len(stale) - len(noop)
    rows.append((f.stem, name or "?", good, len(noop),
                 100.0 * good / max(1, len(en_keys))))

print(f"catalog: {len(en_keys)} keys\n")
print(f"{'lang':<8} {'name':<22} {'translated':>10} {'no-op':>6} {'coverage':>9}")
print("-" * 60)
for code, name, good, noop, cov in rows:
    print(f"{code:<8} {name:<22} {good:>10} {noop:>6} {cov:>8.0f}%")

if errors:
    print(f"\n{len(errors)} ERROR(S):")
    for e in errors[:40]:
        print(f"  {e}")
    if len(errors) > 40:
        print(f"  ... and {len(errors)-40} more")
    sys.exit(1)
print("\nAll dictionaries valid.")
