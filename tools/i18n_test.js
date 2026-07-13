// Regression test for the UI's language resolution and t().
// Run:  node tools/i18n_test.js        (exits non-zero on failure)
//
// It pulls the real functions out of ui/index.html rather than copying them, so the
// test cannot drift from the shipped code. Only SF_LANGS is replaced, so the cases
// stay fixed as languages are added.
const fs = require("fs");
const path = require("path");

const page = fs.readFileSync(path.join(__dirname, "..", "ui", "index.html"), "utf8");
const m = page.match(/<script>(\/\* i18n-runtime \*\/[\s\S]*?\/\* end-i18n-runtime \*\/)<\/script>/);
if (!m) { console.error("i18n runtime not found in ui/index.html"); process.exit(1); }
let rt = m[1];
rt = rt.slice(0, rt.indexOf("var SF_LANG=_pick();"));   // drop the bootstrap (needs a DOM)
rt = rt.replace(/var SF_LANGS=[\s\S]*?\];/, "");        // the test supplies the list

var SF_LANGS = [{ code: "en" }, { code: "en-GB" }, { code: "en-AU" }, { code: "fr" },
                { code: "de" }, { code: "pt" }, { code: "pt-BR" }, { code: "nb" }];
eval(rt);   // defines _D, t, _tr, _match, _walk

let fails = 0;
const eq = (got, want, why) => {
  const ok = got === want;
  if (!ok) fails++;
  console.log(`  ${ok ? "PASS" : "FAIL"}  ${String(why).padEnd(52)} ${JSON.stringify(got)}`);
};

console.log("language resolution:");
// The subtle one: an en-US browser must land on the BASE English (the text already in
// the page), never on en-GB just because it is the only other "en" on the list.
eq(_match("en-US"), "en",    "en-US -> en (base), not en-GB");
eq(_match("en-GB"), "en-GB", "en-GB -> exact variant");
eq(_match("en-AU"), "en-AU", "en-AU -> exact variant");
eq(_match("fr-CA"), "fr",    "fr-CA -> fr (base of the region)");
eq(_match("pt-BR"), "pt-BR", "pt-BR -> exact (not pt)");
eq(_match("pt-PT"), "pt",    "pt-PT -> pt");
eq(_match("de-AT"), "de",    "de-AT -> de");
eq(_match("EN-gb"), "en-GB", "case-insensitive");
eq(_match("ja-JP"), null,    "unsupported -> null (page stays English)");
eq(_match("pl"),    null,    "outside Windows-1252 -> null");
eq(_match(""),      null,    "empty -> null");

console.log("t() keeps the whitespace of concatenation fragments:");
_D["Error:"] = "Erreur :";
_D["Sent"] = "Envoyé";
eq(t("Error: ") + "x", "Erreur : x", 't("Error: ") + x');
eq(t("Sent ") + "5",   "Envoyé 5",   't("Sent ") + 5');
eq(t("Unmapped"),      "Unmapped",   "missing key -> English fallback");
eq(t(null),            null,         "null-safe");

console.log(fails ? `\n${fails} FAILED` : "\nall passed");
process.exit(fails ? 1 : 0);
