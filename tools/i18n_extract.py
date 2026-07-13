#!/usr/bin/env python3
"""Extract the translatable strings from ui/index.html into ui/strings/en.json.

The key IS the English text. That is safe here: the UI has no homographs -- every
string that repeats, or appears under a different tag, means the same thing in each
place ("Save", "Loading...", "Calibration" as both a nav link and a heading). Keying
by text means the markup needs NO data-i18n tagging, so the static page is translated
with zero edits to it.

Two runtime mechanisms, so two kinds of key:

  * TEXT NODES -- translated by a DOM walk + a MutationObserver, so this covers both
    the static markup AND the markup the JS builds later (module cards, the flap map,
    monitor rows). Nothing to wrap: the observer sees them when they are inserted.

  * t("...") -- for messages the JS COMPOSES ("Error: " + e). A DOM walk only ever
    sees the composed result ("Error: Bad JSON"), which is not a key, so these must be
    wrapped at the source.

Wrapping is CONTEXT-AWARE, never a global literal match, because JS strings that look
like prose are often code: 'nav a' is a querySelector, and 'HOME         ' is a padded
bus-monitor decode label whose width keeps the log columns aligned. Wrapping either
would break the page. Only literals inside a .textContent assignment, confirm() or
alert() are touched -- those are user-facing by construction.

Run: python3 tools/i18n_extract.py            # write ui/strings/en.json
     python3 tools/i18n_extract.py --wrap     # ALSO wrap the JS literals in t()
"""
import html
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
UI = ROOT / "ui" / "index.html"
OUT = ROOT / "ui" / "strings" / "en.json"

PREFIX = re.compile(r'^[^0-9A-Za-zÀ-ɏ]+')   # leading icons/arrows: not part of the key
STR = re.compile(r'"([^"\\\n]*(?:\\.[^"\\\n]*)*)"|\'([^\'\\\n]*(?:\\.[^\'\\\n]*)*)\'')

# Contexts whose string literals are, by construction, shown to the user.
UI_CTX = re.compile(
    r'\.textContent\s*=\s*([^;]+);'
    r'|\.placeholder\s*=\s*([^;]+);'
    r'|\b(?:confirm|alert)\s*\(([^;]+?)\)\s*[;)]',
    re.S)


def strip_prefix(t):
    m = PREFIX.match(t)
    return t[m.end():] if m else t


_ESC = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "'": "'", "\\": "\\", "/": "/", "0": "\0"}


def js_unescape(s):
    r"""Decode a JS string literal's escapes.

    The source is scanned as TEXT, so a literal written "Homing…" yields the six
    characters \,u,2,0,2,6 -- but at runtime t() is handed the DECODED string ("Homing…").
    Key on the raw form and the lookup silently misses and the string stays English, which
    is exactly the kind of bug that never announces itself. So the catalog key is the
    decoded text; only the *source* literal stays raw (that is what gets wrapped).
    """
    def rep(m):
        c = m.group(1)
        if c[0] == "u":
            return chr(int(c[1:], 16))
        if c[0] == "x":
            return chr(int(c[1:], 16))
        return _ESC.get(c, c)
    return re.sub(r'\\(u[0-9a-fA-F]{4}|x[0-9a-fA-F]{2}|.)', rep, s)


# Bus-monitor decode labels. The monitor renders protocol, not chrome, and the runtime
# skips #log entirely -- so these would never be used even if they were in the catalog.
DENY = {"HOME OFFSET", "TOTAL STEPS", "NUDGE OFFSET", "DUP ID?"}


def is_key(x):
    """A translatable, user-facing string -- not code, glue, or a protocol label.

    Both sources are user-visible BY CONSTRUCTION -- text between tags, and literals in a
    .textContent/confirm()/alert() -- so this stays permissive on purpose. An earlier
    version rejected anything matching an identifier pattern, to screen out CSS classes
    and element ids; that also rejected "Save", "Cancel", "Refresh", "Home All"... i.e.
    most of the buttons in the app, which then silently stayed English in every language.
    Single words are the norm for buttons, so identifier-shaped is NOT disqualifying.
    """
    x = x.strip()
    if x in DENY:
        return False
    if not (2 <= len(x) <= 240):
        return False
    if not re.search(r'[A-Za-z]{2}', x):            # needs letters, not just digits
        return False
    if re.search(r'[<>{}]|\|\||&&|=>|;', x):        # markup / code
        return False
    # HTML the JS builds by concatenation arrives with its angle brackets already split
    # off ("input id='cfgFlapCount' type='number' value='"), so the check above misses it.
    # An attribute assignment is the tell -- prose never contains one.
    if re.search(r"""=['"]""", x):
        return False
    # camelCase / lowercase identifiers with no space: "logrow", "wallMeta", "sel".
    # A real button reads "Save" or "Go" -- capitalised, or upper-case like "ON".
    if re.fullmatch(r'[a-z][A-Za-z0-9_]*', x):
        return False
    if "/" in x and " " not in x:                   # a path or url
        return False
    return True


def is_protocol_label(raw):
    """Padded, upper-case bus-monitor decode labels ('HOME         ') -- the padding
    keeps the monitor's columns aligned and the words are protocol, not prose."""
    return raw != raw.strip() and raw.strip().isupper() and len(raw.strip()) >= 3


def text_nodes(markup):
    """Text between tags, plus the translatable attributes.

    Entities MUST be decoded first. The icon buttons are written "&#x21bb; Refresh", and
    the browser hands the DOM "↻ Refresh" -- so keying on the raw source would produce
    "x21bb; Refresh" (the prefix scanner stops at the 'x'), which matches nothing. Decode,
    then strip the icon, and the key is "Refresh" -- exactly what the runtime looks up.
    """
    out = set()
    for t in re.split(r'<[^>]+>', markup):
        # A fragment of JS-built HTML often starts mid-tag, because the tag was split
        # across a concatenation:  "...onclick=\"f('" + sn + "')\" title=\"...\">Home</button>"
        # The piece before </button> is therefore  ...">Home  -- it still carries the tail of
        # the opening tag. Keep only what lies after the last '>' and before the first '<',
        # which is the actual text. Without this the piece contains '>' and is-key rejects
        # it, and the Provision tab's Home button silently never became translatable.
        if ">" in t:
            t = t.rsplit(">", 1)[1]
        if "<" in t:
            t = t.split("<", 1)[0]
        t = strip_prefix(html.unescape(t).strip()).strip()
        if is_key(t):
            out.add(t)
    for a in re.findall(r'(?:placeholder|title)="([^"]+)"', markup):
        a = html.unescape(a).strip()
        if is_key(a):
            out.add(a)
    return out


def scripts_of(page):
    return re.findall(r'<script>(.*?)</script>', page, re.S)


def collect(page):
    """Returns (text_node_keys, js_literal_keys). The second set is what gets wrapped."""
    markup = re.sub(r'<style>.*?</style>|<script>.*?</script>', '', page, flags=re.S)
    nodes = text_nodes(markup)

    js = set()
    for sc in scripts_of(page):
        if "i18n-runtime" in sc:
            continue                                  # never rewrite the runtime itself
        # (a) HTML the JS builds -> its text nodes are seen by the MutationObserver.
        #     Unescape first: the source has  title=\"...\"  and the tags are only
        #     well-formed once the backslashes are gone.
        for a, b in STR.findall(sc):
            lit = a or b
            if "<" in lit and ">" in lit:
                nodes |= text_nodes(js_unescape(lit))
        # (b) mrow("Last Seen", value) builds one row of the module-info dialog. The label
        #     is a bare argument -- no tags, no .textContent -- so neither rule above sees
        #     it, and Serial Number / Last Seen / Current Flap / Steps / Rev were silently
        #     untranslatable. The row IS inserted into the DOM, so the MutationObserver
        #     translates it once the label is a key; nothing needs wrapping.
        for m in re.finditer(r'\bmrow\(\s*(["\'])(.*?)\1', sc):
            lab = js_unescape(m.group(2))
            if "<" in lab and ">" in lab:
                nodes |= text_nodes(lab)              # one label wraps its text in a <span>
            elif is_key(lab):
                nodes.add(lab.strip())
        # (b) literals in a user-facing context -> must be wrapped in t()
        for m in UI_CTX.finditer(sc):
            expr = next(g for g in m.groups() if g is not None)
            for a, b in STR.findall(expr):
                lit = a or b
                if is_protocol_label(lit):
                    continue
                if is_key(lit):
                    js.add(lit)                       # keep the raw literal (spaces matter)
    return nodes, js


def wrap(page, lits):
    """Wrap allow-listed literals in t(), but only where they sit in a UI context."""
    n = 0
    def fix_expr(expr):
        nonlocal n
        def repl(m):
            nonlocal n
            raw = m.group(0)
            lit = m.group(1) if m.group(1) is not None else m.group(2)
            if lit not in lits:
                return raw
            if expr[max(0, m.start() - 2):m.start()] == "t(":
                return raw          # already wrapped -- --wrap must be idempotent
            n += 1
            return f't({raw})'
        return STR.sub(repl, expr)

    out = []
    for part in re.split(r'(<script>.*?</script>)', page, flags=re.S):
        if not part.startswith("<script>") or "i18n-runtime" in part:
            out.append(part)
            continue
        def ctx(m):
            whole = m.group(0)
            expr = next(g for g in m.groups() if g is not None)
            return whole.replace(expr, fix_expr(expr), 1)
        out.append(UI_CTX.sub(ctx, part))
    return "".join(out), n


def main():
    page = UI.read_text(encoding="utf-8")
    nodes, js = collect(page)

    if "--wrap" in sys.argv:
        page, n = wrap(page, js)          # wrap matches the RAW source literal
        UI.write_text(page, encoding="utf-8")
        print(f"wrapped {n} literals in t(...)")

    # ...but the catalog key is what t() actually receives at runtime: the decoded string.
    keys = sorted(nodes | {js_unescape(k).strip() for k in js if is_key(k)})
    OUT.parent.mkdir(parents=True, exist_ok=True)
    cat = {"$name": "English"}
    cat.update({k: k for k in keys})
    OUT.write_text(json.dumps(cat, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")

    print(f"text-node keys : {len(nodes):>4}   (DOM walk + MutationObserver)")
    print(f"t() literals   : {len(js):>4}   (composed messages)")
    print(f"catalog        : {len(keys):>4} keys, {sum(len(k) for k in keys):,} B English")


if __name__ == "__main__":
    main()
