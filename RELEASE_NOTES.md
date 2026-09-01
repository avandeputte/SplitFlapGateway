# Split-Flap Gateway — Release Notes

## v3.12.0 — 2026-09-01

**Restore on boot.** A calibration backup can now live on the gateway and be replayed to
every module on every boot — for walls whose modules lose or corrupt their EEPROM, or get
swapped around. The backup is the source of truth; every power cycle puts the wall back.

### Added

- **Restore on Boot** (Settings tab) — upload a backup file (the one *Backup Calibration*
  produces) to the gateway, tick *Restore calibration on every boot*, set the post-boot delay
  (default 10 s, so the modules finish their own auto-home first), and Save. A run writes each
  module by serial number (`mXW`, the same frame `/api/flap/restorebysn` sends, then a fixed
  gap for the EEPROM write), re-provisions it (`mXI`) when the backup's ID differs from the one
  the gateway knows, reads every module back (`mXA` on v25+, `mXD` on v15+, `m<id>d` below
  that) and compares home offset, steps per revolution, every flap position and the v31+ flap
  set, re-writes and re-reads anything that differed or did not answer once, then homes the
  wall. **Run Restore Now** does the same without a reboot; **Cancel Restore** stops a run
  after its current step.
- **The bus is locked for the whole run.** From boot (or the moment *Run Restore Now* is
  pressed) until the run ends, every REST endpoint that would touch the RS-485 bus answers
  `503 {"error":"restore in progress"}` and MQTT commands are dropped; the maintenance switch
  and all read-only, settings, status and OTA endpoints keep working. The gateway's own
  background bus traffic (flap-set trickle, stale-module probes, the quiet-time schedule)
  pauses. The dashboard shows a red border and a banner with the phase and progress.
- **It always finishes.** Every wait is bounded and a module that never answers is counted as
  *not answering*, so a missing module can never leave the gateway locked. A restore that
  cannot start (enabled, but nothing usable stored) is logged and skipped without locking.
- REST: `GET /api/restore` (setting, stored file, run progress and counters),
  `PUT /api/restore/backup` (validated before it is accepted, atomic, max 256 KB),
  `DELETE /api/restore/backup`, `POST /api/restore/run`, `POST /api/restore/cancel`;
  `restoreOnBoot` / `restoreDelay` on `GET /api/config` and `POST /api/config/settings`;
  a `restore` object (`state`, `done`, `total`, `startsIn`) on `GET /api/status`.
- The stored backup lives in FATFS as `/restore.bak` (temp file + rename, survives OTA).
- **The module registry is permanent.** A module is never removed for being quiet. One not
  heard from in 24 hours is re-queried (a version query, a few per minute) so its record stays
  current; after four unanswered queries it is left alone for an hour and asked again. Only
  Identify All, De-provision, or a corrupt record removes an entry, so the module list and
  `GET /api/capabilities` are complete from the first second after boot.

### Fixed

- **Browser OTA no longer dies near the end of a full-size image.** The upload callback set the
  CORS header on every received chunk, and `WebServer::sendHeader()` keeps each call as a
  heap-allocated node until the response goes out -- ~1000 chunks ate ~80 KB of a 100 KB heap,
  the connection reset at 1.2-1.3 MB with min-free heap under 1 KB, and nothing was flashed
  (the browser's instant "100 %" is the file leaving the browser, not arriving). The header is
  now set once, on the response. A 1.48 MB image uploads in ~10 s with 80 KB of heap to spare,
  and the `/ota` page now waits for the gateway to come back and names the version it booted.

### Changed

- **A restore writes the flap map entry by entry**, not inside the `mXW` frame. A module writes
  each entry to EEPROM as it parses it and cannot keep up with a long frame at line speed: on
  v31 firmware a 434-byte restore frame left 20 of 42 entries and the module was deaf for ~6 s
  afterwards. The boot restore now sends `mXW` with offset, steps and flap set (which clears
  the map), waits for that erase, then one short `m<id>w<i>:<p>` per entry, paced — the
  wizard's own frames. **`POST /api/flap/restorebysn` and the Settings tab's Restore from
  File do the same**: the endpoint sends the `mXW` (offset, steps, flap set), schedules each
  entry as a paced `m<id>w` frame on the RS-485 task (the batch queue, so the web server never
  blocks), and returns at once with `{entries, perEntry, id, settleMs}`; the dashboard waits
  `settleMs` per module before re-provisioning it or moving on. It takes an optional `id`;
  otherwise the registry's id for that serial is used, and only with no id known at all does
  the map go inline as before. The dashboard also drops a flap set whose character count does
  not match its flap count from an old backup instead of writing it.
- **An inconsistent flap-set tail is ignored.** A v31 `A` reply that lost its tail in transit
  can parse as `64:` plus a few characters. That used to land in the registry, in
  `/api/capabilities`, and in backups — and restoring such a backup wrote the truncated set
  into the module. The parser now treats a tail whose character count does not match its flap
  count as "not reported", and the restore neither writes nor verifies such a set.
- `POST /api/flap/restorebysn` builds its frame through the same `sfBuildRestoreFrame()`
  the boot restore uses.
- `Access-Control-Allow-Methods` now also lists `DELETE`.
- **`pio run -e esp32s3_ota -t upload`** — a preconfigured espota environment in
  `platformio.ini` (`default_envs` keeps plain `pio run` on the USB build). The browser
  upload at `/ota` still fails near the end of a full-size image on this board (heap
  exhaustion, the connection resets and nothing is flashed); espota is the path that works.

## v3.8.0 — 2026-07-14

Everything since **v3.4**, summarised. One client can now drive any wall: a new
`GET /api/capabilities` tells it exactly what a wall can show, and a new
`POST /api/display/cells` sets a whole row through the same contract the **Matrix Portal
Gateway** answers — so the companion drives the physical wall and the LED-matrix emulation
identically. Plus quiet-time blanking, a per-board MQTT identity, safer web OTA, and a
security fix.

> **Upgrading from v3.4:** two things to know. If you use the **Home Assistant** integration,
> the device id changes (see below) — delete the old device. And modules re-report their flap
> set to the gateway over the first couple of minutes after boot (the persistence format was
> bumped); nothing you need to do.

### Added

- **`GET /api/capabilities`** — one call that answers *what characters can this wall show?*
  On a real wall the answer is not one set: every module owns its reel, and firmware v31 lets
  each be told a different one, so the response reports `union` (any module can show it),
  `common` (**every** module can — these genuinely differ on a mixed wall), and `sets`: each
  distinct reel once, with the module ids that carry it (`"0-44,50"`). Colour flaps are listed
  by name; modules too old to report a reel appear under `assumed`, ones not yet known under
  `unknown`. **The Matrix Portal Gateway answers the same URL identically**, so a client never
  has to know which kind of wall it is driving.

  To answer it, the gateway now learns each module's flap set from the v31+ `A` reply and
  **persists** it — asking a 45-module wall costs ~90 s of bus time at 9600 baud, so it is
  cached, filled by a slow background trickle, and re-read only when an `N` command changes a
  reel.

- **`POST /api/display/cells`** — set a run of modules in one call: `{ch | color | blank | skip}`
  per cell, named colours, and `step_ms` cascade pacing. The **same JSON contract** the Matrix
  gateway answers. It is *lenient* — a cell this wall cannot show is skipped, not a 400 that
  discards the row (the response reports `sent` vs `skipped`); only structural errors 400.

### Changed

- **The dashboard speaks 13 languages** plus English, chosen from the browser with a Settings
  override and a `?lang=` parameter, all in one firmware image.
- **Quiet Time now blanks the wall.** Entering quiet homes every reel (the *Home All*
  operation); leaving it restores what each module was showing, or the newest request made
  while quiet. Applies to the schedule, the manual switch, REST, MQTT and Home Assistant alike.
- **The Home Assistant re-skin is complete** — every control clears WCAG AA in both light and
  dark themes, and the module cards, status tiles and bus monitor read the palette tokens too.

### Fixed

- **[Security] A remotely-reachable stack buffer overflow is closed.** The by-serial
  flap-config path (`POST /api/flap/flapconfig` with an `sn`, and the MQTT flapconfig topic)
  built its RS-485 frame from an **unvalidated** serial into a fixed stack buffer, so a long
  serial overran it and corrupted the stack. The serial is now validated before it is used.
- **Every gateway now has its own MQTT identity.** The client id and Home Assistant device id
  were derived from a MAC field that is identical on every ESP32, so two boards on one broker
  evicted each other in a loop. They now use the bytes that actually differ. **The HA device id
  changes as a result — delete the old device.**
- **Web OTA no longer reboots the board mid-flash.** A low-heap watchdog fired during the
  upload — which drives heap low on purpose — and reset onto the old image. The watchdog now
  stands down for the duration of an upload. *(Browser OTA on this 16 MB board remains
  heap-tight near the end of a large image; ArduinoOTA/espota is the reliable path.)*
- **Smaller fixes:** the companion-URL save no longer contends with a firmware upload for the
  flash; `/api/capabilities` returns in ~0.03 s (its response is buffered and its scratch lives
  in PSRAM) and its module-id range list can no longer truncate or mis-order; and the
  double-quote flap is reachable by character (`"` maps to the reel's `q`, which is what
  `/api/capabilities` advertises).

---

## v3.6.1 — 2026-07-13

**Every gateway now has its own MQTT identity.** Two of them on one broker used to knock
each other offline, forever.

### Fixed

- **The MQTT client id and the Home Assistant device id were the same on every board.** Both
  were derived from `ESP.getEfuseMac()`'s *low* bytes — but that value is byte-reversed, so
  its low bytes are the Espressif OUI (`48:27:e2`), which is identical on every ESP32 ever
  made. Two gateways both connected as `splitflap-20E22748`, and **an MQTT broker evicts the
  client already holding a duplicate id**, so the pair disconnected each other in a loop. In
  Home Assistant they collapsed into a single device.

  The id now comes from the MAC bytes that actually differ between boards.

  > **If you use the Home Assistant integration, the device id changes** (`sfgw_20E22748` →
  > `sfgw_E2xxxxxx`). Home Assistant will discover a *new* device — delete the old one. The
  > integration is opt-in and off by default, so this only affects you if you enabled it.

  Found the moment a second gateway was put on the same network. It ships in Matrix Portal
  Gateway v1.2.1 too, where it was verified on hardware.

---

## v3.6 — 2026-07-13

**Quiet Time now blanks the wall.**

### Changed

- **Turning Quiet Time on empties the display.** Every reel is homed to its blank flap as
  quiet time begins — the same operation as the **Home All** button — so the wall goes dark
  and silent for the night instead of freezing mid-message.

- **Turning it off puts the wall back.** What each module was showing is snapshotted before
  the blanking, so the existing end-of-quiet resync restores it. If the host asked for
  something newer while quiet, that wins instead — the newest request is what you get.

  This applies to the schedule and to the manual switch alike, wherever quiet is set from:
  the dashboard, REST, MQTT or Home Assistant.

### Fixed

- **The Quiet Time banner could never be translated.** The string extractor rejects any text
  containing a semicolon (a filter meant to screen out CSS and JS), and the banner had one —
  so it was the single piece of UI prose with no catalog entry, silently stuck in English in
  all 13 languages. Its replacement has no semicolon, and is translated.

---

## v3.5 — 2026-07-13

Makes the dashboard **multi-lingual** — 13 languages plus English, picked automatically
from the browser — and **fixes the v3.4 re-skin**, which had left several controls
literally invisible in the light theme.

> **If you are on v3.4, update.** In the light theme (the default) every secondary button
> — **Refresh**, **Identify All**, **Home All**, **Re-read EEPROM** — renders white text on
> a near-white background (1.3:1 contrast). The destructive-action buttons and the bus
> monitor's frame text are unreadable too. It looks perfect in dark mode, which is why it
> shipped.

### New

- **The dashboard speaks your language.** English plus **British and Australian English,
  French, German, Spanish, Italian, Portuguese (Portugal and Brazil), Dutch, Danish,
  Swedish, Norwegian and Finnish** — the languages whose text the flap modules could
  actually display (the Windows-1252 repertoire), so the dashboard and the wall speak the
  same set.

  The language is chosen the same way light/dark already is: **it follows your browser**,
  with nothing to configure. In order: `?lang=fr` in the URL (which lets the companion
  request a language for an embedded view *without* changing your own setting, so it is
  deliberately not saved) → the Settings override → `navigator.languages` → English.
  The gateway stores **no language state at all** — no config field, no NVS write, no API.

  **Every language ships in the one firmware image**; there is no per-language build and
  switching never means reflashing. That is affordable because only the *words* are
  duplicated, not the page: the dashboard is 109 KB of HTML/CSS/JS but only ~11 KB of it is
  English *text*. A dictionary carries the English key alongside each translation, so it
  compresses to about **9 KB** — all 13 languages together cost roughly **115 KB of flash**,
  under 7% of what was free. The browser fetches just the one dictionary it needs
  (`GET /lang/<code>`, gzipped) and **English fetches nothing** — it is the text already in
  the page, which is also the per-key fallback. So a translation can be *partial*, and a
  missing string simply stays English instead of breaking a screen.

  The **bus monitor is never translated** — it shows RS-485 frames and their decode, which
  is protocol, not prose (and its decode labels are padded to keep the columns aligned).

### Fixes

- **The Home Assistant re-skin left half the UI behind — and some of it was invisible.**
  The re-skin re-points the palette tokens in `:root`, which only reaches components that
  *read* a token. Everything that hardcoded the old dark palette stayed dark on the new
  light page, and one token change had a nasty knock-on effect:

  `--acc` went from a solid navy to 12% black, but buttons inherit `color:#fff` from the
  base rule — so **every button that overrode only its background kept white text**. In the
  light theme that is white-on-`#e0e0e0`: **1.3:1, invisible**. It hit the secondary buttons
  (Refresh, Identify All, Home All, Re-read EEPROM), the nudge buttons, Tune's GOTO and the
  wizard's Back. The destructive-action buttons were `#212121` on `#1a0d0d` — **1.2:1**.

  | | before (light) | after |
  |---|---|---|
  | Secondary buttons | 1.3:1 — invisible | **12.2:1** |
  | Destructive buttons | 1.2:1 — invisible | **15.4:1** |
  | Bus-monitor frame text | 1.2:1 — invisible | **15.4:1** |
  | Amber status text | 2.0:1 | **4.9:1** |
  | Green status text | 3.3:1 | **5.1:1** |

  Also themed: the **bus monitor** (its log panel now follows the theme, and its
  RX/TX/REST/MQTT channel colours — chosen for a black panel, and 1.7–3.6:1 on white — got
  per-theme shades), the calibration module picker, character map, edit box, note banner,
  EEPROM map, wizard progress bar and the maintenance banner. `.wall` and `.flap` stay dark
  on purpose: they draw the flap wall.

  Semantic colours were doing double duty as *fills* (buttons/banners, which carry their own
  text colour and were fine) and as *text*, where the fill shades collapse on a white page.
  Text usages now take their own tokens. **Every fixed element clears WCAG AA (≥4.5:1) in
  both themes**, verified by computing contrast rather than eyeballing.

- **Mojibake in the OTA help text** — a mangled em-dash was rendering as `?`.

### Under the hood

- **`src/web_ui.h` is now generated.** The dashboard is built from `ui/index.html` by
  `tools/build_ui.py`, which also embeds the language dictionaries. **Edit `ui/index.html`,
  not `src/web_ui.h`** — the latter is regenerated and your edits would be overwritten.
  `tools/i18n_check.py` validates the dictionaries and `node tools/i18n_test.js` covers the
  language-matching rules.

---

## v3.4 — 2026-07-12

Gives the dashboard the **companion's look**, folds **backup & restore** into the
Settings page, teaches the gateway and the companion to **tell each other which tabs
they have**, and fixes a group of real robustness bugs — most importantly a **web
server that froze during a paced batch** and a **watchdog reboot on large walls**.
Drop-in upgrade: no module, MQTT or wiring changes, and backups themselves are
untouched.

### Changed

- **A new look — the Home Assistant design language.** The dashboard is restyled to
  match the companion (which ships the same look), so the two feel like one product.
  It follows the browser/OS **light or dark** preference; there is no theme setting.
  Purely cosmetic — no control moved or changed behaviour.

- **Backup & restore moved to Settings.** The **Backup Calibration** and **Restore
  Calibration** cards now sit at the end of the Settings page; the **Backup** tab is
  gone. Nothing about the backup format or the restore-by-serial behaviour changed —
  only where the controls live. (The backup file is assembled in the browser from the
  existing module/EEPROM endpoints; restore posts to `/api/flap/restorebysn`.) An old
  `#backup` deep link now lands on Settings rather than breaking.

- **Batch pacing no longer blocks the web server.** `POST /api/rs485/batch` used to
  produce its `step_ms` stagger by **sleeping between frames**, on the web task — and
  the HTTP server handles one connection at a time, so a paced batch froze the whole
  web UI for the length of the cascade (up to the old 8 s cap) while further
  connections piled up in the TCP accept queue holding window buffers. The handler now
  stamps each frame with a due time, hands it to the RS-485 task (which already wakes
  every 5 ms) and returns immediately. The cascade on the wall is unchanged.

  Two contract notes: **`200` now means *accepted*, not *transmitted*** (`sent` counts
  frames accepted), and the 8 s total-pacing cap is replaced by a queue depth of **127
  paced frames in flight**. A frame goes out immediately instead of paced when
  `step_ms` is 0, when it is longer than 48 bytes, or when the queue is full — so a
  cascade beyond 127 paced frames loses its stagger past that point. A whole-page
  redraw is one frame per module, so that only bites above 127 modules.

- **The companion URL is persisted on a debounce.** Two companions pointed at one
  gateway each re-register their own URL on their ~30 s heartbeat, so the stored URL
  flipped back and forth — and saving on every change meant **an NVS write every ~30 s,
  forever** (observed in the wild). The URL now applies to RAM instantly (the tab is
  live at once) but only reaches flash once it has held still for two minutes. A
  contested URL therefore never gets written at all, which is the right answer; a
  companion re-registers within a heartbeat of any reboot, so nothing is lost.

### New

- **Tab advertisement (`POST /api/companion`).** The companion may now send `tabs` —
  the deep links its own UI offers — and the response always carries `gwTabs`, this
  firmware's. Each side then renders the other's real tabs instead of a list hard-coded
  on the far side, which is what made the Backup tab above a two-release problem.

  Both halves are optional and independent, so **every old/new pairing works**: an older
  companion advertises nothing and the dashboard falls back to its built-in companion
  tabs; an older gateway returns no `gwTabs` and the companion falls back to its own list
  (which still includes **Backup**, because a pre-3.4 gateway really does have that tab).

  An advertised list is taken **whole or not at all** — any malformed entry, a bad id
  (not `[A-Za-z0-9_-]{1,24}`), a label over 24 printable ASCII characters, more than 10
  tabs, or a list over 384 bytes drops the list, and the peer then shows its built-in one.
  The list is runtime-only (no flash writes: the companion re-sends it on every heartbeat)
  and is cleared when the companion deregisters.

### Fixes

- **Watchdog reboot on a large wall.** `GET /api/flap/modules` sent one chunk per
  module. Each chunk could block on a slow client for up to the 3 s socket timeout, so
  ~41 modules × 3 s exceeded the 120 s web-stall threshold — the supervisor logged
  `STALL: Web=0` and **rebooted the gateway**. It never fired below ~40 modules, which
  is why it hid for so long. The response is now coalesced into ~1400-byte chunks, feeds
  the watchdog on each flush, and aborts early if the client has gone away.

- **A changed MQTT broker now takes effect.** `POST /api/config/mqtt` saved the new
  host/port but never told PubSubClient about it, so the client kept dialling the
  **boot-time** broker and failed forever with `rc=-2` until a reboot. It now re-points
  the client and resets the failure counter.

- **TCP connect timeouts were never actually set.** Several call sites used
  `setTimeout()`, which on `NetworkClient` sets the *read* timeout and leaves the
  **connect** timeout at its 3 s default — the MQTT client and three web handlers all
  meant the latter. They now call `setConnectionTimeout()`.

- **The dashboard re-downloaded its whole page on every navigation.** `GET /` now sends
  an `ETag` and answers a matching `If-None-Match` with `304 Not Modified` (~53 KB
  saved per navigation); the favicon and logo are cached for a week. The dashboard also
  gates its periodic polls so they can't stack up on the one-connection web server.

### New

- **Tab advertisement (`POST /api/companion`).** The companion may now send `tabs` —
  the deep links its own UI offers — and the response always carries `gwTabs`, this
  firmware's. Each side then renders the other's real tabs instead of a list hard-coded
  on the far side, which is what made the Backup tab above a two-release problem.

  Both halves are optional and independent, so **every old/new pairing works**: an older
  companion advertises nothing and the dashboard falls back to its built-in companion
  tabs; an older gateway returns no `gwTabs` and the companion falls back to its own list
  (which still includes **Backup**, because a pre-3.4 gateway really does have that tab).

  An advertised list is taken **whole or not at all** — any malformed entry, a bad id
  (not `[A-Za-z0-9_-]{1,24}`), a label over 24 printable ASCII characters, more than 10
  tabs, or a list over 384 bytes drops the list, and the peer then shows its built-in one.
  The list is runtime-only (no flash writes: the companion re-sends it on every heartbeat)
  and is cleared when the companion deregisters.

---

## v3.2 — 2026-07-10

Adds a **Home All** button to the web UI. Drop-in upgrade from v3.1 — no API,
MQTT, or module-behaviour changes; the only change is in the dashboard.

### New

- **Home All button.** The Display tab (under the Live Display) and the
  Calibration tab (in the module picker) each gained a **Home All** button that
  homes every module at once — it broadcasts `m*h` via the existing
  `POST /api/flap/home` with `{"id":-1}`, so there's no new endpoint.

---

## v3.1 — 2026-07-09

Lets the **[Companion App](https://github.com/avandeputte/SplitFlapGatewayCompanion)**
store its settings in the gateway's flash, so a companion container becomes
stateless — destroy it, start another on any host, and it restores its
configuration from the gateway. Drop-in upgrade from v3.0: every existing
endpoint, MQTT topic and module behaviour is unchanged, and the additions are
purely a new endpoint pair plus one new config field.

### New

- **Companion settings blob store.** The gateway now offers a small, dumb blob
  store that the companion owns end to end:
  - `GET /api/companion/settings` → the stored `gzip(minified JSON)` body as
    `application/gzip`, or `404` when nothing is stored yet.
  - `PUT /api/companion/settings` → stores the gzipped body **verbatim**; replies
    `{"ok":true,"bytes":N}`.

  The firmware never parses the payload — the companion owns the schema and
  compresses/decompresses at its own end. Writes are **atomic**: the body streams
  to a temp file on the FATFS partition and is renamed over the live copy only
  once the last byte lands, so an interrupted upload cannot corrupt settings that
  were already good. The blob (`/compset.gz`) survives OTA firmware updates, is
  capped at 64 KB (real ones are 1–2 KB), and the companion debounces its writes,
  so this costs the flash almost nothing.

  Errors are `400` (empty or truncated body), `413` (too large), `503`
  (filesystem not mounted) and `507` (write failed) — in every one of them the
  previously stored blob is left untouched.

- **Firmware version in `GET /api/config`.** The response now carries
  `"version"` (e.g. `"3.1.0"`). This is how the companion decides whether a
  gateway is new enough to hold its settings; against a 3.0 gateway the field is
  absent and the companion quietly falls back to storing them locally.

### Notes

- The gzipped body is served as `application/gzip` and deliberately **without**
  `Content-Encoding: gzip` — those bytes are the payload, not a transfer encoding
  of it. Declaring the encoding would make HTTP clients transparently decompress
  the body, and the companion decompresses it itself.
- `OPTIONS` preflights now advertise `PUT` alongside `GET,POST`.
- The v3.0 `/api/companion` registration endpoint, previously undocumented, is now
  described in `openapi.yaml` along with the new pair.

---

## v3.0 — 2026-07-07

Adds **batch RS-485 send** and an automatic **Quiet-Time schedule**, plus a
cleaner dashboard and a more robust bus monitor. Drop-in upgrade from v2.1 — all
existing endpoints, MQTT topics, and module behaviour are unchanged; the
additions are purely new config + endpoints.

### New

- **Batch RS-485 send.** `POST /api/rs485/batch` accepts many frames in one
  request (`{"frames":[…],"step_ms":15}`), each normalized like `/api/rs485/send`,
  with an optional device-side `step_ms` pacing the cascade. A host can now draw
  a whole animated page in a single HTTP call instead of one request per module.
  Capped at 512 frames / 8 s of pacing; feeds the web watchdog during long
  batches.
- **Quiet-Time schedule.** Quiet Time can turn on/off automatically on a daily
  schedule. Configure it under **Settings → Quiet Time Schedule** (enable, a
  start/end time, and the days it applies). The schedule is evaluated once a
  second against the RTC and toggles Quiet Time as local time crosses the window
  (overnight windows supported). Transition-based, so a manual toggle within a
  window is respected until the next boundary. Persisted in NVS.
  - `GET`/`POST /api/quiet/schedule` → `{enabled,start,end,days}` (`days` is a
    bitmask, bit0=Sun … bit6=Sat).

### Web UI

- **Cleaner top bar.** The maintenance checkbox and the IP address were removed
  from the header, which now shows just the logo and version badge. Maintenance
  mode is still signalled by the yellow border and banner — and that banner now
  carries a one-click **Turn Off Maintenance** button. (The gateway's IP remains
  on the **Status** page.)

### Fixes

- **Valid JSON on the bus-monitor MQTT topics.** A frame containing a `"` or `\`
  could emit malformed JSON on the `<prefix>/rx` and `<prefix>/tx` monitor topics
  (those characters weren't escaped). They are now escaped; the MQTT and
  web-monitor encoders share one transcoder; and the MQTT buffer was enlarged so
  a full frame of accented / multi-byte glyphs is never truncated mid-message.

### Companion app

- **[SplitFlapGatewayCompanion](https://github.com/avandeputte/SplitFlapGatewayCompanion)
  is released.** A content engine — apps, playlists, schedules and triggers — that
  drives the display over this release's batch endpoint and registers itself with
  `POST /api/companion`, which makes a **Companion** tab appear on the gateway. It
  requires v3.0 or newer. See [Companion App](README.md#companion-app).

### Compatibility & upgrade notes

- No breaking changes: existing REST endpoints, MQTT topics, and backups from
  earlier versions are unchanged. Upgrade the gateway over-the-air as usual
  (Settings → firmware update), then confirm the version badge in the header
  reads **v3.0**.

## v2.1 — 2026-06-29

A bug-fix release that makes the **colour flaps** work correctly end to end and the **calibration tools honour a module's custom flap set**. It is a drop-in upgrade from v2.0 — all endpoints, MQTT topics, and module behaviour are unchanged.

### Fixes

- **Colour flaps are no longer turned into letters.** The seven colour flaps are addressed by the lowercase letters `r o y g b p w` (red, orange, yellow, green, blue, pink, white), but the gateway was normalising *all* lowercase ASCII to uppercase before sending — so a request for blue (`b`) went out as the letter `B`. Lowercase letters are still uppercased to match the default reel, **except** those seven colour codes, which now pass through verbatim. Ordinary text is unaffected (`hello` → `HELLO`); lowercase is now meaningful for colours (`b` → blue flap).
- **Calibration Wizard and Character Map now reflect a module's custom flap set.** Both were hardcoded to the default 64-flap reel, so a module on firmware v31+ with a custom character set or flap count was still calibrated against `A B C …`. They now read the module's live character set and flap count (via the combined `A` dump / `/api/flap/all`) and use them throughout — the map labels, the Wizard's per-flap glyph and progress, the whole-board walk, and the default per-flap step positions (spaced by the module's actual flap count, not a fixed 64).
- **Live Display renders colour flaps.** A flap currently showing a colour now appears as a colour swatch on the Live Display wall, matching the Character Map, instead of printing the bare colour letter.
- **Lowercase is visible on the Display tab.** The Send Text and Send Single Character inputs no longer force-uppercase what you type on screen, so you can enter and see the lowercase colour codes you are sending.
- **The web UI no longer serves a stale page after an update.** The embedded page is now sent with `Cache-Control: no-cache`, so a normal reload always loads the new UI after a firmware flash — previously the browser could keep serving the cached HTML/JS, making an update look like it had no effect.

### Compatibility & upgrade notes

- No API, MQTT, or wiring changes; backups are unaffected. The custom-flap-set calibration display requires module firmware **v31+** (older modules use the fixed 64-flap default reel, exactly as before).
- Upgrade the gateway over-the-air as usual (Settings → firmware update), then confirm the version badge in the header reads **v2.1**. Because of the caching fix, do **one** hard refresh (Cmd/Ctrl + Shift + R) after this upgrade; subsequent updates refresh on a normal reload.

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
