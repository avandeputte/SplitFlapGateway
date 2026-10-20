# Split-Flap Gateway — Release Notes

## v4.0.0 — 2026-10-20

**A major version, because the gateway now does something it never could: update the modules
themselves.**

### What changed, and why that needs a new major version

Until now, new module firmware meant a programmer, a cable and a trip to every single module.
Module firmware [**v35**](https://github.com/avandeputte/SplitFlapUniversalFirmware/releases/tag/35)
changes that by giving each module a **bootloader** — a kilobyte at the start of its own flash
that can replace the rest of the firmware from an image sent over the RS-485 bus. The modules
stay on the wall; the firmware comes to them.

That only works with a controller that speaks the update protocol, and **this release is that
controller**. The gateway grew a new role here: it is no longer only a thing that drives a
display, it is the thing that maintains the hardware behind it. Hence 4.0.

**What you need to do once.** The bootloader itself has to arrive the old way: one last flash
over UPDI per module, with the web flasher or avrdude, which also sets the fuse that reserves
the bootloader's kilobyte. After that, every future module firmware goes over the bus from this
screen. Modules still on older firmware keep working exactly as before — the gateway skips them
and they are not disturbed by the update traffic.

**Nothing about an update puts a module at risk.** It keeps its ID and its calibration, because
the bootloader never writes EEPROM. It never starts a half-written image: a module that loses
power, or loses the gateway, part way through simply waits for the image to be sent again.

### The gateway side

This follows the module firmware's BUS-UPDATE.md step for step, with `tools/busflash.py` there
as the reference sender.

### How a run goes

Upload `firmware-update.bin` from a module firmware release — 15,360 bytes, whose own CRC the
gateway checks before storing it — then start the update from **Modules ▸ Module Firmware**,
either for named modules or for every eligible one. The gateway then:

1. **skips anything older than v35**, which has no bootloader, and says so rather than sending
   it 15 KB it cannot use;
2. **puts the targets into update mode** with a checksummed command, which is the only form a
   module accepts for this, so line noise can never take a wall off the air;
3. **confirms each one is waiting** in its bootloader;
4. **broadcasts the image's 240 pages once**, to every module at a time, so four modules cost
   the same as one;
5. **asks each module which pages it actually wrote and read back**, and resends only the gaps,
   per module, for up to five passes;
6. **tells them to start the image**, which a module does only when it holds every page and the
   whole image matches its CRC;
7. **waits for each one to come back** and reports the version it now runs.

The bus is held **exclusively** throughout, and every frame goes into the trace, so a failed
update leaves a complete record. A module keeps its ID and calibration — the bootloader never
writes EEPROM — and one that loses power part way through waits for the image rather than
starting a half-written one.

### Measured on a real wall

Nineteen modules, eighteen of them with a bootloader:

| Test | Result |
|---|---|
| **18 modules, v35/v36 → v36** | one pass, no resends, **18 of 18**, **83 s** |
| The v32 module on the same bus | skipped by the eligibility check, and undisturbed by 240 broadcast page frames |
| Bus counters afterwards | 0 discards, 0 rejects, 0 checksum failures, 0 incidents |
| **Cancel mid-send**, 18 modules | stopped **3.2 s** after the request, bus released |
| **Recovery**, a second run after that cancel | **18 of 18**, 82 s |
| **A named subset** (3 of 18) | those three on v36, the other fifteen left on v35 |

Eighteen modules cost the same as one, because the pages go out once to the whole bus. The
same wall module by module would be twenty minutes, or eighteen trips with a programmer.

Also exercised: single-module updates both ways between v35 and v36, the whole run driven from
the dashboard including the upload, the guards (an update is refused while a soak owns the bus,
and refused outright when no target has a bootloader), and the protocol's safe exit — entering
update mode and sending go before any page returns a module to the firmware it already has,
with nothing written.

**What a cancel leaves behind.** Once a module has taken its first page it no longer has a
valid firmware, so a cancelled run leaves its targets waiting in their bootloaders with their
reels blank, and the eight-second bootloader watchdog makes them forget which pages they had.
That is the bootloader refusing to start a half-written image rather than anything going wrong,
and the fix is simply to run the update again: measured above at 82 s for eighteen modules.

**One number worth recording:** a module takes **28.7 seconds** to answer again after it
starts a new image. It restarts, re-checks the whole image, waits out the staggered start-up
that keeps a wall from drawing its inrush at once, and homes its reel, and it answers nothing
through any of that. An earlier build called a perfectly good update a failure by asking after
two seconds.

### Added

- `PUT /api/modules/firmware` stores the image (size and CRC checked), `GET` reports what is
  stored, `DELETE` forgets it.
- `POST /api/modules/firmware/start` `{"ids":[…]}`, or `{}` for every eligible module.
- `GET /api/modules/firmware/status` — phase, pass, pages, and a row per module.
- `POST /api/modules/firmware/cancel`.
- `POST /api/modules/firmware/probe` `{"id":N}` asks one module's bootloader how it is doing:
  which image it holds and how many pages of it. A module running its firmware does not answer,
  which is itself the answer.
- **Modules ▸ Module Firmware** in the dashboard: upload, choose modules or all, progress, and
  a table of what each module ran before and runs now.

## v3.18.0 — 2026-10-19

Module firmware **v33** adds a checksummed frame format and four counters of its own. This is
the gateway side of both. A wall can be mixed: the gateway decides per module, from the version
that module reported, and v32 modules see exactly the traffic they saw before.

### Added

- **Checksummed frames.** A frame can be sent as `~HHHH<frame>`, where `HHHH` is the
  CRC-16/CCITT-FALSE of the frame. A v33 module verifies it before acting, drops it if it does
  not match, and answers with checksummed replies; older firmware skips the prefix and runs the
  frame unchecked, so the same frame is safe on a mixed wall.

  The gateway uses it where a corrupted frame does damage or costs a retry — **reads, writes,
  restores and the version query** — and leaves **display content plain**, because five extra
  bytes on a six-byte display command is 80% more traffic for a frame the next update
  overwrites anyway.

  **Every reply that arrives with a checksum is verified**, and one that fails is dropped
  rather than parsed, which is the entire point. It is counted, logged as an incident naming
  the module, and recorded in the soak trace byte for byte. Our implementation was checked
  against the module firmware's own check value and against Python's `crc_hqx`.

  Note for anyone reading the monitor: the receive path previously began at the `m` of a frame,
  so a checksum prefix was skipped and the frame was parsed as though it had never been
  checked. It now starts at the `~`.

- **The counters a v33 module keeps about itself** — receive overflows, broadcast replies sent,
  how many went out late, and checksummed frames it dropped — are read and shown in the
  module's Info dialog and returned by the API. Older modules report them as unavailable.

- **`POST /api/flap/stats` {"id":N}** returns that snapshot **without starting a self-test**.
  The existing diagnostics endpoint returns the same figures but spins the reel for twenty
  seconds with them, and reading a counter should not cost a motor test. Nothing it sends
  moves a flap.

- **Bus Health counts checksummed traffic**: frames verified against frames dropped as corrupt.
  Failures should be zero.

### Measured on a mixed wall

Seven modules on v32, eight on v33:

| | |
|---|---|
| Soak | 18 passes, 256 queries, **0 failures**, 0 discards, 0 rejects, 0 retries |
| Checksummed frames | **165 verified, 0 failed** |
| v32 modules | plain traffic throughout, unchanged |

And the cross-check the module firmware authors suggested, which is only possible now that both
sides count: modules 11 and 13 each reported sending **3** broadcast replies over a run, and the
gateway heard **3** from each. Receive overflows, late slots and checksum errors were all zero.

## v3.17.1 — 2026-10-19

### Changed

- **The gateway stops shouting serial numbers at a wall that knows its own IDs.** Reading a
  module's calibration was addressed by serial number whenever one was known —
  `mXD304C55333832E686300D` at 24 bytes where `m12d` does the same job in 6. Every module on
  the bus receives every command, so those 20 extra bytes were spent by the whole wall, on
  every read, to disambiguate something that is almost never ambiguous.

  Measured on the trace of a two-minute soak, before and after:

  | | Command frames | Bytes on the wire |
  |---|---|---|
  | Before | 575 | 5,742 |
  | After | 590 | 2,058 |

  **64% fewer command bytes**, with the same 240 queries answered and no retries needed.

  The serial-number form has not gone away; it has moved to where it earns its length. A read
  is asked by ID first, and only if that goes unanswered is it asked again by serial number,
  which no other module can answer by mistake. That is also exactly when a stale or duplicated
  ID is a plausible explanation, so the expensive form is now used in the one case it helps.

  It matters beyond tidiness: shorter commands mean fewer bytes every other module has to
  receive while it is busy, and an overrun receive buffer on a module is the mechanism behind
  the silent-module reports this series of releases has been chasing.

  Provisioning (`mXI`, `mXH`), restores (`mXW`), the by-serial API endpoints and the soak's
  pre-run snapshot still address by serial number, each for a reason: the module has no usable
  ID yet, the ID is being changed, or pairing the right calibration with the right serial is
  the point of the operation.

### Also in this release

Written after the module firmware authors replied to the hardening requests, and after
measuring their claims against a live wall of v32 modules.

- **A broadcast batch now ends when every module has answered**, instead of running to a clock.
  A module answers in its own timed slot, and its clock runs slow while bytes are on the bus,
  so later slots land later than nominal. A window sized to the nominal slots closes while the
  last modules are still waiting their turn, and the next broadcast cancels a reply that has
  not gone out. At 64 IDs that window was 46 s against a last reply landing past 50 s, which
  fits the field report of modules going missing *only* in broadcasts better than collisions
  do. The deadline now allows for the drift as a backstop. Measured on 15 modules: the roll
  call holds the bus 1.6 s instead of 2.5, and the combined-dump batch 10.5 s instead of 14.9.

- **A restore will not replay an impossible flap position.** Values such as 65368 (which is
  −168 stored unsigned) exist in older backups; writing one back stores nonsense, and module
  firmware v33 refuses it silently, so the only symptom would be a verify mismatch nobody can
  explain. Such an entry is now skipped and logged with its module and flap.

- **The three bus counters module firmware v33 reports are read and shown** — receive
  overflows, broadcast replies sent, and how many went out late — in the module's Info dialog
  and on `GET /api/flap/diag/status`. Older modules report them as unavailable rather than
  breaking the parse, which was checked against v32.

- **The incident log distinguishes a recovered fallback from real silence.** "A long read
  failed and a shorter one worked" is the signature of a length problem on a module that is
  plainly alive, and it no longer reads as "no reply".

- **A self-test nobody collects no longer blocks every later test.** The running flag was only
  cleared by reading the status endpoint, so starting a Hall test from a script and never
  polling it left the gateway refusing probes and soaks until it rebooted. Stale jobs are now
  reaped past their own deadline, late enough that a polling client still sees its result.

- **Terminators stay on every command but one.** The module authors expected the stats and dump
  queries to suffer the same half-duplex race as the version query, where the module answers on
  the command letter while the gateway is still transmitting its newline and deaf. Measured on
  a v32 module: `m5v` answered 40 times out of 40 and `m5v\n` zero times out of 40, while
  `m5Q` answered 40 out of 40 either way, because its reply takes milliseconds to prepare. So
  the exception stays exactly one frame wide, with the numbers recorded beside it.

## v3.17.0 — 2026-10-19

### Added

- **The soak test records every frame, and the trace is downloadable.** A report says what
  happened; a trace says what was on the wire — which reply came back short, what the bytes
  were, how long the gap before the retry was. That is the difference between guessing at
  someone else's wall and reading it.

  The trace is kept after every run, **clean or not**, because a trace from a healthy wall is
  the baseline a broken one is read against. Download it from the Status tab under the soak
  result, or `GET /api/flap/soak/log` (add `?meta=1` for its size without fetching it). It
  survives a reboot and a firmware update, and is replaced by the next run.

  The file opens with the gateway version, the time, the bus settings and **every module with
  its firmware version and serial number**, so it stands on its own for someone who has never
  seen that wall. Then one line per frame: milliseconds since the start, direction, and the
  frame, recorded **whole** — a full combined dump with its entire calibration map appears in
  the trace exactly as it arrived.

  Coverage was checked against the gateway's own frame counters: **every frame on the wire is
  in the file**, 453 of 453 in a one-minute run.

  Two things shape how it works. **Nothing is written to flash while the test runs** — flash
  writes stall the cache and the test is measuring milliseconds — so the trace accumulates in
  PSRAM and is written once, after the bus is released. And **memory is bounded with the
  newest data winning**: the ring holds 4 MB, evicts whole lines from the front, and records in
  the file how many it dropped.

  Measured rather than estimated: a soak costs about **2 KB per module-minute**, so the biggest
  run most people will do — 64 modules for half an hour, around 58,000 frames — comes to
  **3.6 MB and fits whole**. A full hour, or a wall past a hundred modules, keeps the most
  recent 4 MB and says so in the file.

  Cutting long replies was tried first and dropped: measured against a real run it saved 29%,
  while costing the one thing a trace is for.

## v3.16.0 — 2026-10-18

The board carries 8 MB of PSRAM and was using 2.8% of it, while the things that matter when
something goes wrong — the bus history, the record of what failed — were sized for a board
without any. This spends that memory where it buys evidence.

### Added

- **The bus monitor keeps 512 frames instead of 64.** A minute or more of heavy traffic rather
  than a few seconds, so an intermittent fault leaves a record whether or not anyone had the
  Monitor tab open. It costs 550 KB of PSRAM.

  A reader is handed at most 64 frames per request and resumes where it left off, so a long
  history never turns into a half-megabyte response or a stalled web task. If frames were
  overwritten before a reader came back, the log says so in its own row rather than leaving a
  silent hole.

- **A record of what the bus got wrong**, on the Status tab under Bus Health and at
  `GET /api/bus/incidents` (`DELETE` to clear). A counter says six frames were discarded; this
  says which module had the bus, what it was being asked, how big the frame was and when.
  Discarded frames, replies that would not parse, reads that only answered on the second ask,
  reads never answered, and IDs that look like two modules are all recorded, 1024 of them, in
  PSRAM. That is the question I could not answer for a 64-module wall last week without
  reading two firmwares.

- **`GET /api/status` reports the memory** the board actually has: PSRAM size and free, the
  sketch size and the space left in the OTA slot, beside the existing heap figures and task
  stacks. All of it was visible only on a USB console before.

### Changed

- **A backup may now be 1 MB** rather than 256 KB. A fully calibrated module dumps around 700
  bytes, so a 255-module wall produces roughly 180 KB and the old ceiling was within sight of a
  real installation. The file is parsed in PSRAM; a board without PSRAM keeps the old limit.
- **Dumps asked for over REST now resync and retry** like the soak's do, which is the path the
  dashboard's own Backup Calibration sweep uses.

### Fixed

- **The clock task had the thinnest stack in the gateway**, about 750 bytes spare, and it has
  overflowed once before. It now has a kilobyte more.

## v3.15.2 — 2026-10-18

Hardening of the bus reads, from a 64-module wall whose soak test reported two modules that
answered every short query and never once answered a dump.

### The finding behind it

A module hears every other module's traffic. A calibration dump is around 700 bytes and a
module's receive ring is 64, so **every long reply overruns the receive buffer of every other
module on the bus.** The module firmware handles that safely but bluntly: once bytes are lost
it drops everything it reads until a line terminator arrives, which means the next command
addressed to it is the one that gets eaten. The module then looks silent while it is merely
resyncing, and a test that asks once and moves on records it as dead.

### Changed

- **A bare terminator goes out before every long read.** One byte, about a millisecond, and it
  clears that discard state on every module listening, so the command that follows is parsed
  rather than swallowed.
- **A long read that goes unanswered is asked once more** before it counts as a miss, and the
  soak reports how often the second ask was the one that worked. On a healthy wall that is
  zero; a climbing count means the bus is overflowing module receivers.
- **Longer gap after a long reply** (260 ms, over the module's 200 ms parser idle timeout)
  where short queries keep the old 150 ms.
- **Broadcasts are asked in chunks of sixteen IDs** rather than the whole wall at once. On 64
  modules a combined-dump window was 45 seconds, long enough for slot timing to drift until
  replies landed on each other; that is what the same report showed as collisions, and as
  frames discarded with no terminator. Chunks also let a cancel out sooner.
- **The receive ceiling is 1024 bytes**, up from 768. A module's longest possible reply is 720,
  so 768 left nothing for a reply spliced onto the tail of another, and that wall was
  discarding frames at exactly 767.
- **The soak records evidence per module, not just a verdict**: the firmware version, how many
  frames the gateway discarded while that module had the bus, and how many reads needed the
  second ask. The report now states what was observed — every short reply arrived and no long
  one did, with or without bytes being discarded — and offers the next diagnostic step instead
  of asserting a cause. The earlier wording blamed the module's calibration map for being too
  large to send, which the module firmware does not support: it assembles a dump into a fixed
  720-byte buffer and would truncate, not fall silent.

### Added

- **`GET /api/status` reports why the board last rebooted** (`reset`: POWERON, SW, PANIC,
  TASK_WDT, BROWNOUT and so on) and the stack headroom of the soak and probe tasks alongside
  the permanent ones. Both were visible only on a USB serial console before, which is no use
  when the board is on a wall.

### Fixed

- **The soak test could reboot the gateway.** Its task ran in 4 KB and the additions above took
  the peak to about 4.2 KB, which overflows the stack and panics the board mid-run. It now has
  6 KB, and the headroom is in `/api/status` so the next one is visible before it bites.

## v3.15.1 — 2026-10-18

Two fixes to the bus hold that 3.15.0 introduced. Both were reported within a day of the
release, and both come down to the same thing: a hold meant to protect the operator's work got
in the way of it instead.

### Fixed

- **Homing was refused from the Calibration tab**
  ([#7](https://github.com/avandeputte/SplitFlapGateway/issues/7)). Opening that tab holds the
  bus, and homing was classified as display content, so *Home All* and *Home Motor* answered
  `503 maintenance in progress -- display content is paused` — on the one screen where homing is
  needed most. Homing is maintenance, not content: it paints nothing and it is how you check a
  home offset. It is now allowed whenever the bus is held for operator work, and refused only
  while the bus is reserved outright (a restore) or while a machine-run test is measuring it,
  where an extra frame would spoil the result.

  The dashboard tags its own requests so the gateway can tell the operator from a content
  driver, and that tag not reaching the firmware is what made the failure look arbitrary. The
  gateway no longer depends on it alone: **the client that took a hold is remembered by
  address**, so whoever holds the bus keeps working even behind a proxy that drops unknown
  headers, from a page served before the tag existed, or from a second operator tool.

- **A cancelled soak test could leave the wall paused.** Cancel only set a flag, and the hold
  was released at the end of the soak task, so the wall stayed paused for as long as the task
  took to notice — and the task slept through whole waits at a time. The worst of them is a
  broadcast reply window, one slot per module, which on a wide wall is half a minute. Anything
  that stopped the task reaching its release at all left the bus held until the lease ran out,
  up to three minutes, which one user cleared by power-cycling the gateway.

  **Cancelling now frees the bus before the call returns.** Every wait inside a run is
  cancel-aware, so the task wakes out of a reply window or a pacing delay at once, lets the
  line go quiet, and releases. `POST /api/flap/soak/cancel` answers only once that has
  happened, and reports `busFree`, with `heldBy` naming anything else that still holds the bus
  (an open Calibration tab, the manual switch) so "not free" is never a mystery. Measured on a
  15-module wall: **under 400 ms**, including a cancel in the middle of a broadcast dump.

  The one thing a cancel waits for is a value being written back. The soak nudges a
  calibration value and puts it back; the put-back always completes, because leaving a module
  changed is a worse outcome than a cancel that takes another moment.

  Underneath, every hold the firmware takes for itself is also checked once a second against
  the job that justifies it: no job, no hold. That is the backstop for a job that crashes
  rather than cancels. The snapshot sweep that runs before a soak stops at the cancel too,
  instead of finishing the wall first.

### Changed

- **The banner says whose hold it is.** While your own Calibration tab holds the bus it reads
  *"while this tab is open, display content from elsewhere is paused. Homing and calibration
  still work."* — 3.15.0 said maintenance was in progress, which read as the gateway deciding to
  go into maintenance mode on its own.
- **There is a way out of any hold**: the banner carries a **Release the bus** button for every
  hold except a restore, which must be cancelled instead. Over REST,
  `POST /api/bus/release {"reason":"…","force":true}`; forcing the soak's hold cancels the soak
  rather than orphaning it.
- **A tab nobody is using stops holding the bus.** The dashboard renews its lease only while the
  tab is visible and has been touched in the last ten minutes, and releases it when the page
  goes away.
- **Display content is paused for everyone, the dashboard included, while the probe, the soak or
  a module self-test is running.** Those measure the bus; a character painted mid-run is a frame
  the measurement did not account for.

### For anyone maintaining this

`tools/bus_gate_test.py <gateway-ip>` runs the whole matrix against a live gateway: which
requests are allowed under each kind of hold, that a lease expires on its own, that a cancelled
soak frees the bus within seconds, and that a stuck hold can always be cleared. It homes module
0 a few times and writes nothing. Both bugs above are in it as named checks.

## v3.15.0 — 2026-10-18

Maintenance that happens by itself, and a soak test for the whole wall.

### Added

- **The bus hold.** Provisioning, calibration, the module self-tests, the probe, a backup sweep
  and a restore now take a hold on the bus automatically — nothing to remember, no switch to
  flip. While a hold is in force, display content is paused: MQTT display commands are dropped
  and the display endpoints answer `503` to any caller that is not the gateway's own dashboard,
  while the operator keeps working. The banner names what is holding the bus and for how long.
  A restore takes the stronger form, which refuses everything until it is done.

  Every hold carries a lease and expires on its own, so an abandoned job or a closed browser tab
  can never leave the wall locked. The manual **Maintenance** switch is the one hold without
  expiry, and it now shares the mechanism — which closes a gap it always had: it gated MQTT only,
  so a companion driving the wall over REST painted straight over a calibration in progress.
  The dashboard tags its own requests (`X-SF-Client: dashboard`); anything without the tag is
  content. The **Calibration tab holds the bus for as long as it is open.**

  `GET /api/bus` shows the holds; `POST /api/bus/hold {"reason","seconds"}` takes or renews a
  lease (capped at 300 s) and `POST /api/bus/release` drops it; `GET /api/status` carries
  `bus.lock`.

- **A whole-wall soak test**, on the Status tab under Bus Health. Pick 5, 10, 20 or 30 minutes
  and it works the bus seven ways, pass after pass, keeping score per module per kind:

  | Pass | What it sends | What it catches |
  |---|---|---|
  | Roll call | `m*v<lo>-<hi>` — every module answers in its own timed slot | a reply that is missing, damaged, or arrives **twice** for one ID |
  | Version, snapshot | `v`, `Q` — the short replies, one module at a time | power and contact faults, which hit every size equally |
  | Dump, combined dump | `d`, `A` — the long replies | a line that loses long frames |
  | Broadcast dump | `m*A<lo>-<hi>` — the longest replies, back to back | slot timing under the worst load the bus ever sees |
  | Write round trip | the home offset and one map entry, each nudged one half-step, read back, **put back**, and read back again | a write that does not land, or an EEPROM that no longer holds one |

  **Before it writes anything it reads every module's calibration into the gateway's flash**,
  so the run can be undone from the gateway alone: one button puts the whole wall back, through
  the same per-entry restore the boot restore uses, and the snapshot can also be downloaded and
  kept. It is a separate file from your own stored backup, which it never touches.
  `GET /api/flap/soak/snapshot` (add `?meta=1` for just what is in it),
  `POST /api/flap/soak/rollback`.

  Nothing it sends moves a flap, and display content is paused for the run. The report is a
  list of findings, not a scoreboard: each one names the modules, the evidence, and what to do
  about it — long frames lost but short ones fine reads as termination and cable; misses at
  every size as power or contact at those modules; misses only in broadcasts as replies
  colliding; an ID that answers twice as two boards sharing it; a value that could not be put
  back is reported first, with the original value to write back. `POST /api/flap/soak
  {"minutes"}`, `…/soak/status`, `…/soak/cancel`. The probe, the self-tests and the soak refuse
  to run over one another.

  A module whose own values could not be confirmed restored keeps them in the report, so
  nothing is lost silently, and the rollback above covers the whole wall.
  `tools/soak_sim.py` in the repo models a flaky bus — termination,
  contact, a duplicated ID, a worn EEPROM, a silent module — and prints the status JSON, so the
  report can be worked on without a wall to break.

### Found while testing

- **A module on firmware v32 whose calibration map is completely full can no longer send its
  EEPROM dump at all.** It answers `v` and `Q` as usual, and `d`/`A` never arrive, until the map
  is erased. This is the field report "the module stopped answering `mXA`, erasing the EEPROM
  fixed it", reproduced deliberately on eight modules. The soak reads that signature out of its
  ordinary passes — short replies 100%, long replies 0%, not intermittent — and says so, with
  the recovery: erase the map and restore from a backup, which writes the entries one at a time.
  The lasting fix belongs to the module firmware, which assembles the whole reply before
  sending it.

## v3.14.0 — 2026-10-17

Bus diagnostics. A module that stops answering used to look the same whether it was silent or
whether it answered and the gateway threw the reply away. These changes tell those apart.

### Added

- **A bus reply-size probe**, on the 🩺 diagnostics dialog. It asks one module for four replies
  of increasing size — version (~32 B), stats snapshot (~19 B), EEPROM dump, and the combined
  dump, which is the longest reply a module ever sends — repeats each a few times, and reports
  which came back and how big they were. The verdict is in plain language: everything answered;
  or short replies get through and long ones do not, which points at the bus rather than the
  module; or nothing answered, which is power, wiring or the wrong ID. `POST /api/flap/probe`
  and `GET /api/flap/probe/status`. It refuses to run while a motor self-test or a calibration
  owns the bus, since a probe alongside one reports a healthy module as silent.
- **Bus Health on the Status tab** — frames discarded, parse rejects, and the longest complete
  reply received since boot, also on `GET /api/status` as `bus`. Discards and rejects should sit
  at zero; if they climb, suspect the wiring before the modules.

### Changed

- **A discarded frame is now counted and logged.** A reply that overruns the receive buffer, or
  never reaches its newline, used to be dropped in silence — indistinguishable from a module
  that said nothing, which is exactly the case that is hard to debug. It now increments a
  counter, records its size, and prints `[RX] discarded N bytes with no terminator`.
- **`GET /api/flap/all` falls back to the shorter `v`+`d` when the combined `A` read times out.**
  It used to return stale on the assumption that a timeout means the module is offline, which is
  wrong whenever the failure depends on reply *length*: `A` grows with the calibrated flap map
  (600–750 bytes on a fully calibrated module, against a 768-byte ceiling) and a bus that cannot
  carry it will often still carry `d`. This is the difference between a backup that silently
  skips modules and one that gets them. The response reports `mode` as `A>vd` when it happened.
- **The bus monitor reports a frame's true length** and marks a row that was stored truncated.
  The ring holds 256 bytes per frame, so a 447-byte reply was displayed cut short with nothing
  saying so — misleading in exactly the situation you would be using the monitor to investigate.

## v3.13.2 — 2026-10-17

### Fixed

- **The timezone setting was ignored: the Status page always showed UTC**
  ([#2](https://github.com/avandeputte/SplitFlapGateway/issues/2)). The NTP sync fetches UTC with
  `configTime(0, 0, server)`, and the ESP32 core's `configTime()` ends by writing the `TZ`
  environment variable itself — so it overwrote the configured zone with `UTC0DST0`, and every
  local-time conversion after the first sync returned UTC. Re-picking the zone appeared to work
  and then reverted, because changing it also forced an NTP re-sync, which clobbered it again.
  The zone is now re-applied after every sync, through a single `rtcApplyTZ()` that holds the
  time mutex (`setenv`/`tzset`/`localtime` are not thread-safe in newlib, and the time is
  formatted from other tasks). Changing the zone no longer forces a re-sync: the clock is stored
  in UTC, so the zone only affects formatting.

  Affected the Status page clock and the `wt` wall-clock field on bus-monitor and MQTT frames.
  The Monitor tab was never wrong — it renders the UTC epoch in your browser's timezone.

- **The Home, Info and Diagnostics icons on a module card were invisible in light mode**
  ([#4](https://github.com/avandeputte/SplitFlapGateway/issues/4)). The dashboard's generic
  button rule sets white text, and `.micon` overrode the background, border and padding but
  never the colour — so the icons were white on a white card, a contrast ratio of 1.00. Only the
  destructive-actions icon was visible, because it sets its own red. They now take the theme's
  text colour, which also covers their SVGs (drawn with `currentColor`). Dark mode is unchanged.

## v3.13.1 — 2026-09-25

### Fixed

- **A Quiet Time schedule could reboot the gateway -- and with Restore on Boot enabled, loop.**
  The schedule was evaluated on the RTC task, which has a 2 KB stack that already peaks at
  about 1.4 KB just reading the clock. Asserting Quiet Time from there sends bus frames through
  a call chain (`sfSetQuietTime` → `sfHome` → `rs485Send` → `mqttPublishMsg`, plus `printf`)
  that needs roughly 1.5 KB more: a stack overflow, a panic, a reboot. Reported as "quiet time
  does not work with restore on boot: it goes blank, after a minute it loads the backup, and
  loops" -- exactly that sequence: power-up homing (blank), the boot restore, the schedule
  asserting quiet the moment the restore released the bus, the crash, and round again. The
  schedule now ticks on the network task, which has the stack for it.
- **Restore on Boot now takes the bus before any task starts.** It used to be armed after the
  tasks were spawned, so the first schedule tick could assert Quiet Time in the gap and the
  restore then ran under it (its final home-all suppressed).

## v3.13.0 — 2026-09-23

### Added

- **A default step-pacing setting (`stepMs`).** Settings ▸ *Display Layout* gains *Default step
  pacing (ms)*, stored in NVS and reported as `stepMs` on `GET /api/config` (set it with
  `POST /api/config/settings {"stepMs":25}`, 0–100, default 15). The companion (v2.10.22+)
  reads it as its default `step_ms` -- the per-module gap every app page send and Compose
  inherit -- so the pacing a wall needs lives with the wall, not in each client. Larger walls
  need slower pacing: measured on 60 modules, 15 ms lost 27, 20 ms lost 9, 25 ms was stable.
  The gateway stores and reports the value; it does **not** pace its own sends by it.

### Changed

- The per-request `step_ms` cap on `/api/rs485/batch` and `/api/display/cells` is raised from
  30 to 100 ms to match, so a stored default above 30 is honoured rather than silently clamped.

## v3.12.1 — 2026-09-14

### Fixed

- **Display layouts of more than about 50 cells broke the Display and Calibration tabs**
  ("Could not load display state", "Error loading layout"). `GET /api/display/state` streams
  its cell list in 256-byte batches, and every batch but the last was sent without a string
  terminator, so stale bytes leaked into the JSON at each batch boundary. A small wall fits in
  one batch and never hit it; a 6 × 20 wall did on every poll. The batch is now terminated
  before each flush. No other endpoint had the flaw (the module list streamer already
  terminated its chunks).

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
