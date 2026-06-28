# Split-Flap Gateway — Release Notes

## v2.0 — 2026-06-28

Adds gateway support for the **runtime-configurable flap set** introduced in **module firmware v31**: the active flap count (1–64) and the ordered character set are now set per module from the gateway, read back, and preserved across backup/restore. It is a drop-in upgrade — every existing endpoint, MQTT topic, and module behaviour is unchanged, and the new controls simply stay inert for modules on older firmware.

### Highlights

**Configurable flap set (requires module firmware v31+)**

Module firmware v31 made the physical flap count and the character order (which character lives at each flap index) configurable at runtime via a new `N` command, persisted in the module's EEPROM, instead of the previously fixed 64-flap built-in set. The gateway now drives that end to end:

- **Module Info dialog** — for a module on firmware **v31+**, shows the live **active flap count** and **character set** (read from the module's `A` dump) and provides an inline editor to change either or both. The flap count and the character set are **independent** — set one and leave the other untouched; leave a field blank to keep the module's current value. The Flap Set section is shown **only** for v31+ modules and is omitted entirely on older firmware.
- **Whole-panel broadcast** — target `id:-1` to push one flap set to every module on the bus in a single command (the calibrated map and other per-module state are untouched).
- **By serial number** — a flap set can also be addressed to a specific module's serial number.
- **Euro sign and accented characters (Windows-1252)** — the flap character set is no longer limited to ASCII. You can use the euro sign `€` (e.g. instead of `$`) and Western-European accented letters (`é à ü ö ä ñ ç ß …`) on the flaps. Characters are entered as normal UTF-8 in the web UI / JSON; the gateway transcodes them to the single-byte Windows-1252 form the RS-485 bus and module firmware use, and back to UTF-8 when reading the set or the live display. Each glyph is one flap; characters with no Windows-1252 representation (emoji, non-Latin scripts) are rejected. This applies to the configured flap set **and** to characters/text sent to the display. No firmware change is required (the module stores raw bytes).
- **Backup & restore round-trip** — a backup now also captures each module's configured flap count and character set (from the v31+ `A` dump), and a restore re-applies them, so a full backup/restore preserves a custom flap set. Modules on older firmware are unaffected (the tail is simply absent).

### REST / MQTT additions

Additive — existing endpoints and topics are unchanged:

| Interface | Endpoint / Topic | Description |
|---|---|---|
| `POST` | `/api/flap/flapconfig` | Configure flap count and/or character set by `id` (`id:-1` broadcasts) or `sn` (firmware v31+) |
| MQTT | `<prefix>/flap/flapconfig` | Same, e.g. `{"id":5,"flapCount":40,"charSet":" ABC…"}` |

`/api/flap/all` now also returns `flapCount` and `flapChars` (`-99` / `""` for pre-v31 firmware), and `/api/flap/restorebysn` accepts an optional `flapCount`/`charSet` to restore the flap set. The combined `A` reply parser reads the new `:<flapCount>:<flapChars>` tail (and, thanks to the forward-compatible parser added in v1.8, older gateways already ignore it safely).

### Compatibility & upgrade notes

- **The configurable flap set requires module firmware v31 or newer.** The Info dialog hides the Flap Set section entirely for modules below v31 (they use the fixed 64-flap default set); the new endpoint/topic still accept requests but an older module ignores the unknown `N` command. Every other feature works with any module firmware.
- No breaking changes: existing endpoints, MQTT topics, and backup files from earlier versions still work (an old backup simply has no flap-set tail to restore).
- Upgrade the gateway over-the-air as usual (Settings → firmware update), then confirm the version badge in the header reads **v2.0**.

## v1.9 — 2026-06-17

A polish and reliability release: a fix for present-but-quiet modules occasionally dropping off the known-module list, a refreshed browser presence (favicon and header logo), and clearer module action icons. It is a drop-in upgrade from v1.8 — all endpoints, MQTT topics, and module behaviour are unchanged.

### Fixes

- **Stale-module purge no longer drops live modules.** Modules unseen past the 6 h staleness window are *probed* before being purged, but the gateway was firing those version-query probes back-to-back. Because a module answers a bare version query **synchronously and instantly**, on the half-duplex bus each reply collided with the next probe and was lost — so present-but-quiet modules (e.g. on an idle display left on one message, where they all go stale together) were dropped despite being "probed." Probes are now **spaced out** and sent in **bounded batches**, so every reply is received and the module is kept. A module is purged only if it genuinely stays silent after a clean probe.

### Web UI

- **Browser icon (favicon).** A split-flap-themed SVG favicon is now served at `/favicon.svg` and shown in the browser tab and bookmarks.
- **Header logo.** The "Split-Flap Gateway" title is now a split-flap *board* wordmark — each letter on its own flap tile, with the seam and pivots of the favicon — served at `/logo.svg` in place of the plain text. It scales down on narrow screens.
- **Clearer module action icons.** The diagnostics (stethoscope) and delete (trash) buttons were color emoji that ignored the UI's colours and rendered low-contrast on the dark theme. They are now monochrome SVG icons that inherit the button colour, so the diagnostics icon is crisp and the delete icon correctly renders **red**, matching its intended destructive-action styling.

### Project

- **In-file license header.** The firmware source now carries the project's Creative Commons Attribution-NonCommercial-ShareAlike 4.0 (CC BY-NC-SA 4.0) notice in-file, with credit to **Adam G Makes** for the split-flap module hardware and the initial protocol.

### Compatibility & upgrade notes

- No API, MQTT, or wiring changes. Upgrade the gateway over-the-air as usual (Settings → firmware update) and confirm the header badge now reads **v1.9**.

## v1.8 — 2026-06-17

This release adds **module hardware self-diagnostics** and a **whole-board calibration** mode, along with the supporting REST API, web UI, and documentation. It is a drop-in upgrade from v1.7 — all existing endpoints, MQTT topics, and module behaviour are unchanged.

### Highlights

**Module self-diagnostics (requires module firmware v26+)**

A 🩺 diagnostics icon now appears on every provisioned module whose firmware is **v26 or newer**. Clicking it runs three built-in self-tests in order and opens a results modal with plain-language interpretation:

- **Stats snapshot (`Q`)** — instant, no motor movement. Reports the last reset cause (decoded from the RSTFR bits into power-on / brown-out / external / watchdog / software), the boot counter, supply voltage, an EEPROM write-verify result, and the current flap index. It highlights a sagging supply, a failed EEPROM verify, or a brown-out / watchdog reset. The supply-voltage check is **rail-aware**, so a healthy 3.3 V module is not mistaken for a low 5 V rail.
- **Hall sensor self-test (`T`)** — spins the reel ~2 revolutions to check the home sensor and reports **OK** or one of **stuck-active**, **stuck-inactive**, **multiple active regions**, or **inverted polarity**, with the rising/falling home-edge counts.
- **Mechanical self-test (`M`)** — spins the motor several revolutions and reports **OK**, **inconsistent** (revolutions varied by more than 5 %, indicating intermittent missed steps from drag, a weak supply, or a failing driver), or **no motion**. It also reports the per-revolution step counts, average magnet width, and motion-gate detail that distinguish a stalled motor from a dead sensor. On **firmware v29+** the rotation count is selectable (**5–20**, default 5) — more rotations catch intermittent faults a short run misses. The Hall and mechanical tests are motor-driven, so they run as asynchronous jobs the UI polls; the snapshot shows immediately.

**Whole-board calibration**

A new **Calibrate Whole Board** mode steps every module through each flap at the same time, so you can walk the entire wall one flap at a time and fix any module that is off using the familiar per-character nudge dialog — no need to calibrate modules one by one. The single-module and whole-board flows now live in one consolidated **Calibration** section. Maintenance mode is left exactly as it was found when you finish.

### REST API additions

Three new endpoints (additive — existing endpoints are unchanged):

| Method | Endpoint | Description |
|---|---|---|
| `POST` | `/api/flap/diag` | Returns the stats snapshot `q` inline and starts the Hall sensor test |
| `POST` | `/api/flap/diag/mech` | Starts the mechanical self-test after the Hall test; optional `revs` 5–20 (firmware v29+, default 5) |
| `GET` | `/api/flap/diag/status` | Polls the active motor test (`kind` hall/mech): `idle` / `pending` / `done` / `timeout` |

All are documented in `openapi.yaml` and the README's REST API section.

### Fixes

- **Rail-aware supply-voltage check** — a healthy 3.3 V reading is no longer reported as a low supply. The threshold now adapts to the rail (warn below ~3.0 V on a 3.3 V module, below ~4.5 V on a 5 V module).
- **Build fix** — a malformed configuration line (two `#define` directives merged onto one line, leaving `MODULE_PROBE_GRACE_MS` swallowed by a comment) broke compilation; the directives are now on separate lines.
- **Diagnostics polling robustness** — the results poller now keeps a single request in flight and fires exactly once, so a test result can no longer be clobbered by a late "idle" poll (which previously showed "test did not complete" even when the module had returned valid data).
- **Forward-compatible `A` parser** — the all-fields reply parser ignores any field a future module firmware appends after the calibration map, instead of folding it into the map.

### Compatibility & upgrade notes

- **Self-diagnostics requires module firmware v26 or newer** (the 🩺 icon only appears on those modules); the **selectable rotation count needs v29+** — on v26–v28 the mechanical test runs the fixed default of 5 rotations. Every other feature works with any module firmware.
- The new API endpoints are backward-compatible; no client changes are required.
- Upgrade the gateway over-the-air as usual (Settings → firmware update), then confirm the version badge in the header.
