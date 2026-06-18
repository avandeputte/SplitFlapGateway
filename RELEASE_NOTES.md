# Split-Flap Gateway — Release Notes

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
