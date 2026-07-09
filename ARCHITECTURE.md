# Architecture Notes

This document captures the reasoning behind non-obvious design decisions in the
Split-Flap Gateway firmware, so future maintainers (including future-you) don't
have to re-derive them.

## Task model

The firmware runs five FreeRTOS tasks plus `loop()`:

| Task | Core | Priority | Responsibility |
|---|---|---|---|
| `taskRS485` | 0 | 3 | Byte accumulator for the RS-485 bus |
| `taskRTC` | 0 | 2 | Reads the PCF85063 RTC every second |
| `taskWeb` | 0 | 2 | `server.handleClient()` loop |
| `taskNetwork` | 1 | 1 | WiFi/MQTT/NTP, status publishing |
| `taskOTA` | 1 | 1 | `ArduinoOTA.handle()` every 100ms |
| `loop()` | — | — | Watchdog check every 30s |

Shared state is protected by four mutexes: `sfMutex` (module registry),
`msgMutex` (RS-485 ring buffer), `timeMutex` (time formatting — newlib time
functions are not thread-safe), and `mqttQMutex` (MQTT outbound queue). Only
`taskNetwork` ever calls `mqtt.publish()`.

## Watchdog

`loop()` checks each task's heartbeat timestamp every 30 seconds and reboots if
any task has gone silent past its threshold (RS485/Net = 30s, Web = 120s). It
also force-reboots if free heap drops below 20KB. Thresholds differ because the
web task can legitimately block for several seconds while streaming a large page
to a slow client.

Two boot-time guards prevent false reboots during startup:

- **60-second grace period.** Stall detection is skipped for the first 60s of
  uptime. The one-time FATFS format (first boot after flashing) and WiFi/MQTT
  bring-up happen in this window and can briefly skew task scheduling; rebooting
  then would be a false positive. The low-heap emergency check still runs during
  the grace period.
- **Future-timestamp guard.** Each heartbeat check treats a timestamp "in the
  future" (`wdg > now`, possible only from transient boot-time skew) as healthy,
  rather than letting the unsigned subtraction underflow to a huge value that
  would look like a 49-day stall.

## Web server: why the synchronous `WebServer`, not `ESPAsyncWebServer`

The firmware uses the Espressif-maintained synchronous `WebServer` that ships
with the ESP32 Arduino core. This was a deliberate choice, re-evaluated after a
`Web=0` watchdog stall was observed (the web task blocked inside
`server.handleClient()` on a half-open client socket — a known limitation of the
synchronous server when a browser opens a speculative TCP connection and never
completes the request).

`ESPAsyncWebServer` would eliminate that specific stall, but it carries its own
trade-offs that, for a single-user home-network gateway, were judged not worth
the risk:

- **Third-party maintenance risk.** The async server is a community library
  (the actively maintained fork is currently `mathieucarbou/ESPAsyncWebServer`),
  not part of the Espressif core. The sync `WebServer` ships with the core and
  has a guaranteed maintenance path.
- **No blocking allowed in handlers.** Async callbacks run on the lwIP (`tiT`)
  thread. The `/api/flap/version` and `/api/flap/dump` endpoints currently wait
  up to ~500ms–1s for a module to reply over RS-485. That pattern is illegal in
  async and would require redesigning those two endpoints to return immediately
  and have the client poll (or a deferred-response mechanism).
- **Tighter stack / thread-safety constraints.** Running on the lwIP thread
  means limited stack for the large `char` buffers used in handlers, and every
  access to shared state (`sfModules`, etc.) crosses a thread boundary —
  mutex mistakes become hard crashes rather than recoverable stalls.
- **Heavier, less predictable heap use.** Async buffers responses and manages
  its own allocations; per-connection transient footprint is larger. After the
  effort spent getting heap dead-stable (see below), the predictability of the
  sync server is a feature.
- **The streaming response needs rework either way.** `handleRoot` streams the
  UI as ~154 `sendContent()` chunks; async uses a pull-based chunked-response
  callback, a different control flow.

### Mitigation in place

Rather than migrate, `taskWeb` proactively force-closes any client connection
that lingers longer than 8 seconds (`c.stop()`), reaping the half-open
speculative sockets (Chrome/Safari behavior) that cause the wedge before they
can block the send path. The 120s web watchdog remains as the safety net. A
watchdog reboot in this scenario is *correct recovery* — a task wedged in a
kernel syscall cannot be interrupted from itself — and the device survived 6.5
hours before the single observed stall, recovering cleanly.

**Revisit trigger:** if `Web=0` stalls recur despite the 8s reaper, the async
migration becomes justified. The real work there is redesigning the two blocking
endpoints, not the boilerplate.

## Memory management

The gateway is intended to run for weeks unattended, so heap stability is
critical. Several issues were found and fixed:

- **`rtcFormatTime` `setenv` leak (root cause of the long-standing drain).**
  The function originally called `setenv("TZ", ...)` twice per invocation to
  convert stored-UTC RTC time to local time. On ESP32 newlib, repeated `setenv`
  leaks heap (~66 bytes/pair). Called every 10s by `mqttPublishStatus` and on
  every browser status poll, this produced the observed ~132 bytes/30s constant
  decline. **Fix:** `TZ` is set exactly once at boot (`loadConfig`) and again
  only when the timezone changes (`handleApiConfigSettings`). `rtcFormatTime`
  now computes the UTC epoch manually (a portable days-since-1970 algorithm,
  since `timegm()` is not exposed in ESP32 newlib) and applies the local zone
  with `localtime_r()` — no per-call environment manipulation.
- **`JsonDocument` in hot paths.** ArduinoJson 7's `JsonDocument` allocates its
  pool from the heap on every call; on ESP32's `dlmalloc` this fragments and
  trends `getFreeHeap()` downward. `mqttPublishMsg` (every RS-485 frame),
  `mqttPublishStatus` (every 10s) and `handleApiStatus` (every browser poll)
  were converted to `snprintf` into stack buffers. The remaining `JsonDocument`
  uses are in on-demand REST handlers, where fragmentation impact is negligible.
- **Per-message `String` allocations** in `ringDrain` and `sfParseResponse`
  were replaced with stack `char` arrays.
- **`handleApiModules`** reserves its output `String` up front
  (`out.reserve(sfModuleCount * 220)`) to avoid realloc churn, and
  `loadUnprovisioned` browser polling was slowed from 3s to 10s (it was doubling
  the `handleApiModules` call rate).

After these fixes, heap holds stable (observed ~139KB steady-state) with only
normal allocator sawtooth that recovers on traffic.

## Sticky module persistence

The known-module registry survives reboots so the dashboard isn't empty after a
power cycle. Storage uses a **FATFS file** (`/modules.dat`) in the FATFS
partition that already exists in the default "16M Flash (3MB APP/9.9MB FATFS)"
scheme -- deliberately NOT a custom partition table or an enlarged NVS region,
to keep flashing simple and avoid erasing config on partition changes.

- Only durable fields are stored (id, serial, provisioned, fwVersion,
  lastSeenEpoch). The transient display state and on-demand EEPROM dumps are not
  persisted.
- `lastSeenEpoch` is an RTC wall-clock timestamp (not `millis()`, which resets),
  so "last seen N hours ago" is meaningful across reboots. Entries older than
  `MODULE_STALE_SECS` (6h) are pruned on load; at runtime (once a minute) a stale
  entry is first **probed** with a version query and dropped only if it stays
  silent past a grace window, so a quiet-but-present module is never purged. The
  runtime probes are spaced out and sent in bounded batches -- a module answers a
  bare version query synchronously, so firing them back-to-back would make each
  reply collide with the next probe on the half-duplex bus.
- Writes are atomic: the file is written to `/modules.dat.tmp` then renamed over
  the live file, so a crash mid-write can't corrupt the existing good copy.
- Saves are debounced (5s) and only triggered when the registry actually changes
  (new module, provision, deprovision), limiting flash wear.
- "Identify All" (`POST /api/flap/identify`) wipes both the in-memory list and
  the file, then broadcasts `m*v` to rebuild from scratch.

`MAX_MODULES` is 255, covering module IDs 0-254 (ID 255 is reserved as the
empty-slot / unprovisioned sentinel). With the EEPROM dump cache removed from
the in-RAM struct (dumps are now fetched on demand), each `SFModule` is ~48
bytes, so the full registry is ~12 KB of RAM and the persisted file is ~9 KB --
both comfortable on the 8 MB-PSRAM / 512 KB-SRAM ESP32-S3. The largest transient
cost is the `handleApiModules` JSON response (~33 KB heap String at 255
modules), which is `reserve()`d up front and freed immediately after each send.

A deliberate design point: the RS-485 **frame buffers** (`MSG_MAX_BYTES` = 256
for the monitor ring, `TX_MAX_BYTES` = 768 for outbound frames, `MQTT_BUF_SIZE`
= 768) are sized by the **largest single frame**, not by module count. The
worst-case frame is a full 64-flap EEPROM restore (`mXW<sn>:<offset>:<steps>:<map>`,
~620 bytes; on firmware v31+ an optional `:<flapCount>:<flapChars>` tail adds up
to ~67 bytes, still well inside `TX_MAX_BYTES`) -- a frame always targets one
module, so raising the module ceiling from 200 to 255 does not change them.
Three-digit IDs (vs two) add at most ~1 byte to a handful of commands, still far
inside `TX_MAX_BYTES`. These buffers were sized for the frame, and the frame is
what bounds them.

## Companion settings blob (v3.1)

The companion app parks its settings on the gateway (`GET`/`PUT
/api/companion/settings`) so its container can be stateless. The gateway is a
**dumb blob store**: the payload is `gzip(minified JSON)` whose schema belongs
entirely to the companion, and the firmware stores the bytes verbatim, hands them
back byte-for-byte, and never parses them. That boundary is the whole point --
the companion's schema can evolve freely without ever touching firmware.

It reuses the FATFS partition and the same temp-file-then-rename atomicity as the
module registry above (`/compset.gz`, via `/compset.tmp`), so an interrupted
upload cannot corrupt a good copy. The blob is capped at 64 KB; real ones are
1-2 KB, and the companion debounces its writes, so flash wear is negligible.
Because it lives on FATFS rather than in the app partition, it survives OTA.

The non-obvious part is **receiving the body at all**. The synchronous
`WebServer` copies a non-form request body into a `String` via its `char*`
constructor, so `server.arg("plain")` stops at the first NUL byte -- and a gzip
stream carries one at offset 3 (the FLG header byte). A 127-byte blob would
arrive as 3 bytes. So the PUT is registered with the four-argument
`server.on(uri, HTTP_PUT, fn, ufn)` overload: supplying that upload callback is
what makes `WebServer` route the request down its **raw** path
(`RequestHandler::canRaw()` is true whenever a handler has a `ufn` and isn't a
GET), streaming the body to the callback in `HTTP_RAW_BUFLEN` (1436 B) chunks
that we append straight to the temp file. The final handler then sends the
response, reading a status the raw callback left behind.

Two consequences worth remembering: the raw callback **cannot stop** the server's
read loop, so a request that fails validation early (too large, no filesystem)
must keep draining chunks and simply drop them, or the response never reaches the
client; and the response is deliberately served as `application/gzip` **without**
`Content-Encoding: gzip`, because those bytes are the payload rather than a
transfer encoding of it -- declaring the encoding would make HTTP clients
transparently gunzip the body, leaving the companion to decompress plain JSON.

## Time handling

The RTC stores UTC. Local time is derived at format time using the configured
POSIX TZ string (DST-aware). The `timeMutex` serialises all time-formatting
calls because newlib's time functions and the `TZ` environment are process-wide
and not thread-safe. The NTP server is configurable (default `pool.ntp.org`);
`configTime` is always called with a zero offset so the system clock is UTC and
`mktime` acts as `timegm`, avoiding double-offset bugs.

## Diagnostics

The periodic `[WDG]` line and the boot banner are the primary field-debugging
surface. Boot prints `esp_reset_reason()` (distinguishing a firmware `PANIC`
from a `BROWNOUT` power fault from a clean `POWERON`) plus a heap/PSRAM/flash
snapshot. The `[WDG]` line, emitted every 30 s, carries free heap, minimum-ever
heap, largest allocatable block and a derived fragmentation %, per-task stack
high-water marks (the early-warning signal for the stack-canary crash class),
bus RX/TX counters, a parse-reject counter (rising = bus collisions or noise),
WiFi RSSI, and MQTT state. Stall reboots name the offending task and its age;
low-heap reboots log the heap value. None of this requires the serial-debug
toggle -- it is always on, because the situations it diagnoses (unexpected
reboots, slow memory leaks, bus corruption) are exactly the ones where you
cannot reproduce on demand.

## Protocol note: dump-by-serial vs dump-by-id

`/api/flap/dump` prefers the serial-number command `mXD<sn>` only when the
module reports firmware **v15 or newer**, because `mXD` does not exist in older
firmware. For older modules (or if the SN-based request times out) it falls back
to the ID-based `m<id>d`. This keeps the EEPROM inspector working across all
firmware versions.

## Module self-diagnostics

The 🩺 diagnostics action (module firmware v26+) runs three tests in the fixed
order **Q → T → M**, reusing the existing reply-capture machinery rather than
adding a parallel path. The **`Q` stats snapshot** is instant and captured
synchronously in the request handler, exactly like an EEPROM dump: arm a wait
slot, send the frame, spin on a ready flag with the web-task watchdog touched.
The **`T` Hall self-test** and **`M` mechanical test** are motor-driven (~2 and
~6+ revolutions), so they share a single async job — only one motor test is ever
in flight. The browser sequences them: `POST /api/flap/diag` returns the
snapshot and starts `T`; once the poll reports `T` done it calls
`POST /api/flap/diag/mech` to start `M`. The `M` job deadline scales with the
requested rotation count (5–20 on firmware v29+), because the motor time does.

The reply frames (`m<id>Q:…`, `m<id>T:…`, `m<id>M:…`) are parsed in
`sfParseResponse` alongside `v`/`d`/`A`, into per-test capture globals; the `M`
parser keeps the trailing comma-separated per-rotation list as its greedy last
field. `GET /api/flap/diag/status` is consume-on-read (it reports `done` once,
then `idle`), so the UI poller is built to match: one request in flight at a
time (self-scheduling, not a fixed interval), firing its terminal callback
exactly once, with a generation counter that cancels any stale poller left by a
new run or a closed modal. That combination is what stops a late `idle` read
from clobbering a result the module actually returned.
