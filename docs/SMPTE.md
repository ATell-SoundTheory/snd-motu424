# MOTU PCI-424: SMPTE (LTC) timecode reader and generator

Recovered by static analysis of the x86_64 slice of MOTU's `MOTUPCIAudio.kext` (see [HARDWARE.md](HARDWARE.md)). Addresses are kext virtual addresses. The Linux driver implements this in `motu424_smpte.c` (§9); none of it has been tested with an LTC or video signal.

The card reads LTC from an analog input (or a 2408mk3 SMPTE jack) and can clock to it, and it can generate LTC on any output. MOTU's "MOTU PCI SMPTE Setup" app is the front end: frame rate (24, 25, 29.97 drop, 29.97 non-drop, 30), source, destination, freewheel and generator.

Notation: `cb+X` = DSP control-block dword (`ND424::DSP::Write` @0x29d80, `Read` @0x29df0,
`WritePolitely` @0x2bbc0 = Write + 1 µs delay (call to IODelay stub, uncertain)). `ND` = ND424 object
(`[Drv+0x4720]`), `Drv` = Drv424 object.

Common symbols used below:

* `R` = base rate: `ND+0x3c` (bool, set by `ND::SetSampleRate48k` @0xa1d0) ? 48000 : 44100.
  Encoded in the code as `cmp byte [ND+0x3c],1; sbb; and; add` (e.g. 0xbb80 + (-3900)).
* `M` = card multiplier `ND+0x1ec` (1, 2, 4; set by `ND424::SetSampleRateMultiplier` @0x2bf70).
* `a` = `ND+0x2a8` = "speed" ×100 (2400, 2500, 2997, 3000).
* `b` = `ND+0x2ac` = "counting format" ×100 (2400, 2500, 2997 = drop-frame, 3000).
* `w` = AudioWire port 0..3, `ch` = channel 0..23 on that wire.

---

## 1. User-facing settings

Settings live in a dictionary property `"SMPTE"` on the engine. `Engine::SetPropertyFromPlugIn`
(@0x9896) gets `"Setting"` + value → `Engine::SetSMPTESetting(OSString*,int)` @0x3260.
`Engine::RestoreConfiguration` @0x3607 replays a saved `"SMPTE"` dictionary through the same function.
`Engine::UpdateSMPTEProperties` @0x53d0 publishes `SMPTESources`, `SMPTEDestinations` (OSArrays of
codes) and a `"SMPTE"` dictionary with all keys below.

| Key | Drv setter / getter | Drv field | Values | Default (`Drv::SetDefaultSMPTESettings` @0xdda0) | Change hook (Drv424) |
|---|---|---|---|---|---|
| `FreewheelAddress` | `SetFreewheelAddressTime` @0xde60 / @0xdec0 | +0x46b8 | int, ms at base rate; <0 = infinite | 0 | `FreewheelTimesChanged` @0x160e0 |
| `FreewheelClock` | `SetFreewheelClockTime` @0xde90 / @0xded0 | +0x46bc | int, ms; <0 = infinite | 0 | same |
| `Source` | `SetSMPTESource(u32)` @0xdf00 / `GetSMPTESource` @0xe620 | +0x46c8 | source code (§2), 0 = none | 0 | `SMPTESourceChanged` @0x16050, then `UpdateSMPTEVideoSyncState` @0xb6c0 |
| `SMPTEGenMode` | `SetSMPTEGenerationMode` @0xdf50 / @0xdf80 | +0x46dc | 0, 1, 2 (§5) | 0 | `SMPTEGeneratorSettingsChanged` @0x19ac0 |
| `Destination` | `SetSMPTEDestination(u32)` @0xe990 / `GetSMPTEDestination` @0xe520, `GetActualSMPTEDestination` @0xe5b0 | +0x46d8 | dest code (§2), 0 = none | 0 | `Drv::SMPTEDestinationChanged` @0xe950 → `SMPTEGeneratorSettingsChanged` |
| `OutputLevel` | `SetSMPTEOutputLevel` @0xdfb0 / @0xdff0 | +0x46e0 | 0..0xffff; **ignored while the destination is dedicated** (`test byte [Drv+0x46d9],1` = dest bit 0x100) | 0x7fff | `SMPTEOutputLevelChanged` @0x15d50 → cb+0xdc |
| `Notifications` | `ClearSMPTEUserNotifications(mask)` @0x13cb0 (write = clear bits) | +0x46f4 | bit0 = generator clobbered audio, bit1 = frame format auto-changed | – | `Engine::PostSMPTEUserNotifications` @0x6bd0 |
| `FrameRate` (read-only in dict) | `GetFrameRate` @0xdd40 | +0x46ac | 0..3 | 3 (only if `SetDefaultSMPTESettings(true)`) | – |
| `FrameFormat` (read-only in dict) | `GetFrameFormat` @0xdd50 | +0x46b0 | 0..3 | 3 (idem) | – |
| `Last30fpsRate` (read-only) | `GetLastRateFor30FrameFormat` @0xdd60 | +0x46b4 | 2 or 3 | 3 (idem) | – |
| `SMPTEGen` (separate property, @0x9a04) | `StartGeneratingSMPTE(u64)` vtbl 0x1d0 / `StopGeneratingSMPTE` vtbl 0x1d8 | – | bool + `SMPTEGenMode` (must equal current mode) + `Offset` (u64) | – | §5 |

`SetDefaultSMPTESettings` then calls vtbl 0x2e0 (FrameRateChanged, only with arg true), 0x2e8, 0x2f0,
0x300, 0x308 and `UpdateSMPTEVideoSyncState`.

`SMPTEHourOffsetChanged` (vtbl 0x338) is an empty function on the 424 (@0x150c0; also @0x2dc00).
No hour-offset setting reaches the hardware.

### 1.1 Frame rate / frame format

Set through the user-client call `UC::SetFrameRate` @0x297b0 → `Engine::SetFrameRate` @0x8c60.
The request is `{u32 op, u32 rate, u32 format}`; 4 in rate/format means "keep current". The engine
also resets the shared-status "bad format" word to 4 (@0x8d3e). Then `Drv::SetFrameRate(rate, format)`
@0xe9c0:

* rate must be 0..3 and format 0..3, else `0xe00002c2` (kIOReturnBadArgument).
* Allowed: `rate == format`, or `format == 3 && rate ∈ {2,3}`.
* Clears notification bit 1 (`ClearSMPTEUserNotifications(2)`), stores +0x46ac/+0x46b0; if format == 3
  also stores `Last30fpsRate = rate`; calls vtbl 0x2e0 = `Drv424::FrameRateChanged` @0x16120.

`Drv424::FrameRateChanged`: table `kFrameRates[]` @0x3e2e0 = `{2400, 2500, 2997, 3000}`;
`ND424::SetFrameRate(a = tbl[FrameRate], b = tbl[FrameFormat])` @0x2bf50 → `UpdateSMPTEParameters`,
then vtbl 0x300 (`SMPTEGeneratorSettingsChanged`, which restarts the generator).

| User format | FrameRate | FrameFormat | a | b |
|---|---|---|---|---|
| 24 fps | 0 | 0 | 2400 | 2400 |
| 25 fps | 1 | 1 | 2500 | 2500 |
| 29.97 drop-frame | 2 | 2 | 2997 | 2997 |
| 29.97 non-drop | 2 | 3 | 2997 | 3000 |
| 30 non-drop | 3 | 3 | 3000 | 3000 |

(No 30-drop and no 23.976. Format 2 means "drop-frame counting". Format 3 means "30-frame counting".
Rate 2 or 3 sets the actual speed.)

`SetLastRateFor30FrameFormat` @0xdd70 stores rate ∈ {2,3} only if the current format is **not** 3.

---

## 2. Source / destination codes

Built by `Drv::RebuildSMPTESourceAndDestinationLists` @0xea70 (called from `DoRefreshChannelInfo`
@0x10fb2). The lists are Drv+0x46c0 (sources) and Drv+0x46d0 (destinations).

```
code = (w << 6) | kind | idx
kind 0x100 : dedicated SMPTE jack on the box on wire w, idx = 0..N-1
kind 0x200 : ordinary audio channel ch (0..23) on wire w
0          : none
```

* Dedicated sources: idx < `iface->NumberOfDedicatedSMPTEInputs()` (slot 0x108). Dedicated
  destinations: idx < `NumberOfDedicatedSMPTEOutputs()` (slot 0x110). Only the 2408mk3 has them (1 each,
  `AW_2408` @0x1d3d0/@0x1d3c0, model == 4).
* Channel sources: `0x200|(w<<6)|ch` for every ch where the input slot exists (`Drv+0x4c+0x2c*(24w+ch)`)
  **and** `iface->IsInputAnalog(ch)` (slot 0x128; for the 2408 @0x1c830). So only analog inputs are
  offered as LTC sources.
* Channel destinations: every output slot with `Drv+0x10cc+0x2c*(24w+ch)` set.
* `GetSMPTESource`/`GetSMPTEDestination` return the stored code only if it is still in the list, else 0.
* `GetActualSMPTEDestination` @0xe5b0: if bit 0x200 is set, replaces the low 6 bits with
  `Drv[+0x10c8 + 0x2c*(24w+ch)] % 24` (physical slot of that output, uncertain meaning).

### 2.1 Mapping to DSP indices

`X(w,ch) = 25*w + 1 + ch` (1..24, 26..49, …). This is the same per-wire base-25 numbering as `cb+0xac`
(`25w+24`, video) and `cb+0x88` (`25w+28`).

| Path | Code → ND call | DSP writes |
|---|---|---|
| Source (`Drv424::SMPTESourceChanged` @0x16050) | 0 → `ND::SetSMPTESource(0,false)`; 0x100 → `(25w+25, true)` (idx ignored); 0x200 → `(X(w, code&0x3f), false)` (no remap) | ND+0x278 = src, ND+0x27c = dedicated; `SendAddressSource` @0x2af50 (§3.1) |
| Destination (`SMPTEGeneratorSettingsChanged` @0x19ac0, uses Actual dest) | 0 → `ND::SetSMPTEDestination(0,0,false)`; 0x100 → `(w+1, 0, true)`; 0x200 → `(X(w,ch), Drv+0x44 ? 12 : 0, false)` | §3.4 |
| Clobber (`Drv424::SetSMPTEClobberChannels` @0x15d80, Actual dest) | 0 → `(…, 0)`; 0x200 → `(X(w,ch), 1)`; 0x100 → `iface->GetChannelsUsedByDedicatedSMPTEOutput(&ch,&n)` (slot 0x120) → `(X(w,ch), n)` | §3.4 |

`Drv+0x44` = "card doubles" (4x mode, see [INTERFACES.md](INTERFACES.md) §5.3). At 4x the second DSP destination slot is
`X+12`.

---

## 3. Control-block writes

### 3.1 Address (LTC reader) source: `ND424::SendAddressSource` @0x2af50

Called by `SetSMPTESource` @0x2b170 and `WordSourceChanged` @0x2aff0. `ws = ND+0x38`.

| cb | Value |
|---|---|
| +0x84 | 1 if ws == 2 (ADAT sync). Otherwise, if `ND+0x278 != 0` and 3 ≤ ws ≤ 7: `ND+0x27c ? 3 : 2`. Otherwise 0. |
| +0xa8 | `ND+0x278` (0, X(w,ch) or 25w+25) |
| +0x114 | 1 (commit) |

So the LTC reader runs only when the card clock is SMPTE (ws 3) or video sync (ws 4..7). With video
sync, the clock comes from video and the address comes from LTC. `cb+0x84 = 2` = LTC from an audio
input; `3` = LTC from the box's dedicated SMPTE input.

### 3.2 `ND424::UpdateSMPTEParameters` @0x2bc60

Called from `SetFrameRate` @0x2bf50, `SetSampleRateMultiplier` @0x2bf70 and `SampleRateChanged`
@0x2d880. The stack slots are constants that depend only on the rate family:

* `[rbp-0x48]` = R (0xbb80 or 0xbb80−0xf3c = 44100) @0x2bd3c
* `[rbp-0x40]` = 10·R (0x75300 = 480000 or 441000) @0x2bd57
* `[rbp-0x34]` = 100·R (0x493e00 = 4800000 or 4410000) @0x2bd72

| Order | cb | Write | Formula | Meaning (inferred) |
|---|---|---|---|---|
| 1 | +0x9c | W | `(b+50)/100` | nominal fps (24, 25, 30, 30, 30); also ND+0x2b8 |
| 2 | +0xa4 | WP | `6*b` | frames per 10 min (14400, 15000, 17982 DF, 18000); ND+0x2bc |
| 3 | +0xa0 | WP | `6*b/10` | frames per minute (1440, 1500, 1798 DF, 1800); ND+0x2c0 |
| 4 | +0xb0 | WP | `b/100` (trunc) | 24, 25, **29 = drop-frame**, 30 |
| 5 | +0xb4 | WP | `((8a << 20)/R + 5)/10` | LTC bit rate per sample, Q20 (80·fps/R·2²⁰) |
| 6 | read +0x44 → `K` | | | DSP-provided constant (uncertain) |
| 7 | +0xb8 | WP | `K*10R / (8a)` (64-bit) | K LTC bit-periods expressed in samples |
| 8 | +0xbc | WP | `floor(100R / a)` | samples per frame, integer part |
| 9 | +0xc0 | WP | `((100R − bc*a) << 32) / a` | samples per frame, fractional part, 0.32 (a≠0 always; the a==0 branch is dead code since bc already divided by a) |
| 10 | read +0x48 → `V` | | | DSP-provided constant (uncertain) |
| 11 | +0xe0 | WP | `M > 1 ? V>>1 : V` | |
| 12 | +0xe4 | WP | `(V*10R + 4a) / (8a)` | V bit-periods in samples, rounded |
| 13 | +0xf0 | WP | `0x40000 / floor(V*R / 64 000 000)` | magic 0x431bde82d7b634db, shr 0x18 → ÷64e6 (uncertain meaning) |
| 14 | +0x114 | WP | 1 | commit |

Worked values (48 kHz family): 30 fps → b4=0xcccd, bc=1600, c0=0; 29.97 → b4=0xcc98, bc=1601,
c0=0x9a029004; 25 → b4=0xaaab, bc=1920; 24 → b4=0xa3d7, bc=2000. At 44.1 kHz: 30 → b4=0xdee9, bc=1470;
29.97 → b4=0xdeb0, bc=1471, c0=0x78b25ab6; 25 → 0xb9c2/1764; 24 → 0xb254/1837, c0=0x80000000.
Only `M` (in +0xe0) depends on the multiplier. All other values use the base rate.

### 3.3 Freewheel: `ND424::UpdateFreewheelTimes` @0x2b040

`ND424::SetFreewheelTimes(addr, clock)` @0x2b0f0 stores ND+0x2b0 = addr and ND+0x2b4 = clock.
`Drv424::FreewheelTimesChanged` @0x160e0 passes `(GetFreewheelAddressTime, GetFreewheelClockTime)`. It is
also called from `SetSampleRateMultiplier` and `SampleRateChanged`.

| cb | Formula |
|---|---|
| +0xc4 | clock < 0 ? 0xffffffff : `clock * (R/100) / 10` = clock × R/1000 → **ms → samples** |
| +0xc8 | addr < 0 ? 0xffffffff : `addr * (R/100) / 10` |

(No commit write.) R/100 = 480 or 441 (the constant is `0x1e0` or `0x1e0−0x27`).

### 3.4 Generator / destination

| Function | cb writes |
|---|---|
| `ND424::SetSMPTEDestination(x, off, ded)` @0x2b1b0 | not ded: cb+0xcc = x, cb+0xd0 = x+off, cb+0xd4 = 0. ded: cb+0xcc = 0, cb+0xd0 = 0, cb+0xd4 = x (= w+1). |
| `ND424::SetSMPTEOutputLevel(l)` @0x2b190 | cb+0xdc = l (0..0xffff, 0x7fff default; probably linear gain, uncertain) |
| `ND424::SetSMPTEClobberChannels(x, n)` @0x2b280 | cb+0xd8 = n==1 ? `(x<<16)\|x` : n==2 ? `(x<<16)\|(x+1)` : 0; then cb+0x11c = 0 |
| `ND424::StopStriping` @0x2b360 | cb+0x118 = 1 |
| `ND424::SetSMPTEGeneratorStartTime(t)` @0x2d8d0 | `SR = R*M`; `f = t*a / (100*SR) + 1`; cb+0xe8 = f; `off = ((f*100*SR − t*a) << 16) / a` = (first sample of frame f − t) in Q16 samples; cb+0xec = (M == 4) ? off>>1 : off. No commit write. |
| `ND424::GetSMPTEGeneratorFrame` @0x2b380 | reads cb+0x4c (current generated frame count) |
| `ND424::DidSMPTEGenClobberAudio` @0x2b300 | reads cb+0x11c; if ≠0 writes 0 and returns 1 |

`t` is a sample time at the actual rate. The frame number sent is therefore `ceil`-ish of `t·fps/SR`.
The time code emitted is "frames since sample 0", so the host chooses `t` = TC position × SR (uncertain:
the frame → hh:mm:ss:ff conversion is done by the DSP with cb+0x9c/0xa0/0xa4/0xb0).

---

## 4. Status / readback

### 4.1 Polling (Drv424::HandleInterrupt @0x19c30)

The buffer has 3 interrupts (cb+0x64 = 3). `phase = DMA position / Drv+0x4698` selects the work:

* **phase 0**:
  * If 3 ≤ ws ≤ 7: `Drv::SetTransportState(ND424::GetSMPTEState())` @0x19d0c → @0x13180.
  * Always: `d = ND424::GetDetectedFrameRate()` @0x19d22 (reads cb+0x40, then writes 0 to it).
    `24 → SMPTEFormatDetected(0)`, `25 → 1`, `29 → 2` (drop), `30 → 3`. Any other value (0 = nothing new) is ignored.
* **phase 1**: `UpdateSMPTEGenerator(slot, now)` @0x15e40 with `now` = vtbl 0x2d8 timestamp + 16·Drv+0x466c, then `ND424::SetLockAndTach(now)` @0x2d9f0.
* **phase 2**: `DidSMPTEGenClobberAudio()` → `InstallSMPTEUserNotification(1)` @0x13cc0 (only once per destination change; armed by Drv+0x46f8). If the generator is running (Drv+0x20dab): `GetSMPTEGeneratorFrame()` → `ReportSMPTEGenFrame` @0x13860 if changed → `Engine::ReportSMPTEGenFrame` @0x7cf0 (shared status +0x14).

### 4.2 `ND424::GetSMPTEState` @0x2a830 → `TransportState`

`s34 = cb+0x34` (LTC address reader state), `s38 = cb+0x38` (clock/video state; read only if ws ≠ 3).

| ws | Condition | Return |
|---|---|---|
| 3 (SMPTE clock) | ND+0x278 == 0 (no source) | 1 |
| 3 | s34 == 1 | 3 |
| 3 | s34 == 2 | 4 |
| 3 | s34 == 3 | 5 |
| 3 | other | 1 |
| 4..7 | s38 == 1 and s34 == 1 | 3 |
| 4..7 | s38 == 1 and s34 == 2 | 4 |
| 4..7 | s38 == 1, other s34 | 6 |
| 4..7 | s38 == 2 and s34 ∈ {1,2} | 4 |
| 4..7 | s38 == 2, other s34 | 5 |
| 4..7 | other s38 | 1 |

`Drv::SetTransportState` @0x13180 stores Drv+0x4658. Entering state 2 (not from 3) arms a 2 s deadline,
and `Drv::HardwareInterrupt` @0x13f3c then promotes 2 → 3. States 2..4 count as "rolling": the
interrupt timestamps (@0x13ee3) are sent to the engine, and generator mode 1 runs (§5). State 1 is the
initial value (@0x1168e). It is also set by a MIDI sysex from a box (@0x1328f).

Meaning (**uncertain**, no strings in the kext): 1 = stopped / no LTC; 2 = starting (host-side only);
3 = locked & rolling; 4 = rolling (s34 == 2, maybe freewheeling); 5 = s34 == 3 (freewheel expired?),
not rolling; 6 = clock reference OK but no address, not rolling.

### 4.3 Detected format → `Drv::SMPTEFormatDetected(fmt)` @0x13d10

* If fmt == current FrameFormat: nothing.
* Else, if an engine client exists, `Engine::BadSMPTEFormatDetected` @0x2a80 writes fmt into the
  shared status word +0x10 (native + byteswapped copy) and returns true. The user-space plug-in decides.
  4 = "none" (reset by Engine::SetFrameRate).
* Otherwise the driver auto-switches: `Drv::SetFrameRate(rate, fmt)`, with rate = fmt for 0, 1, 2 and
  `Last30fpsRate` for 3. It then posts notification bit 1 (`InstallSMPTEUserNotification(2)`).

---

## 5. Generator control (Drv424)

Fields: Drv+0x20db0 (u64 start sample time), +0x20dab (running), +0x20dac (wanted).

* `StartGeneratingSMPTE(t)` @0x15760: start = t, running = 0, wanted = 1. `StopGeneratingSMPTE`
  @0x15780: wanted = 0. The base `Drv` versions are empty.
* `SMPTEGen` property (@0x9a04): true and mode == 2 → `Engine+0x208 = Offset`, `Engine+0x200 = 1`. At the
  next `Engine::HardwareInterrupt` (@0x8798) it calls `StartGeneratingSMPTE(engine_sample_time + Offset)`.
  True and mode ≠ 2 → `StartGeneratingSMPTE(Offset)` immediately. False → `StopGeneratingSMPTE`.
* `TransportStateChanged` @0x15790 (vtbl 0x2f8): in mode 1, wanted = (state ∈ 2..4).
* `UpdateSMPTEGenerator(slot, now)` @0x15e40 (phase 1), with `P = Drv+0x4670` (samples per interrupt,
  ×2 at 4x, @0x138cd):
  * mode 1 while running: if start == now, start = now + 48·P; else running = 0 (forces a re-sync).
  * If running ≠ wanted:
    * wanted: mode 1 → start = now + 48·P; mode 2 → start += 16·P; mode 0 → start unchanged. Then `ND::SetSMPTEGeneratorStartTime(start)` and running = 1.
    * not wanted: `StopStriping` (cb+0x118 = 1), running = 0.
  * Writes the "just started" flag into the host status buffer `Drv+0x4678[slot]`.
* Modes (**names uncertain**): 0 = start at an absolute sample time, 1 = follow transport/regenerate
  (chases the incoming timeline), 2 = start relative to the engine sample clock.
* `SMPTEGeneratorSettingsChanged` @0x19ac0 runs after a destination, mode or frame-rate change:
  1. running = 0, `StopStriping`.
  2. Program the destination (§2.1).
  3. For every present box: `iface->EnableDedicatedSMPTEOutput(w == destWire && dest is dedicated)` (slot 0x118).
  4. If mode == 1, 1 ≤ ws ≤ 7 and transport ∈ 2..4: wanted = 1.
  5. Tail-call `SetSMPTEClobberChannels` (vtbl 0x1e0).

---

## 6. Box (AudioWire interface) hooks

Base `AW_WireInterface` versions @0x1a540..0x1a590 return 0 / do nothing. `AW_2408` (2408mk3 = model 4,
`+0x78 == 4`):

| Hook | Address | Effect |
|---|---|---|
| `HasVideoSyncInput` | @0x1d3b0 | model == 4 |
| `NumberOfDedicatedSMPTEInputs/Outputs` | @0x1d3d0/@0x1d3c0 | model == 4 ? 1 : 0 |
| `EnableDedicatedSMPTEOutput(on)` | @0x1ca60 | `+0x76 = on`; `FlushSetup` → mk3 state byte 15 bit 0x02 |
| `GetChannelsUsedByDedicatedSMPTEOutput(&ch,&n)` | @0x1c8c0 | if config byte 1 bit 6 (bank C = S/PDIF): ch = 16, n = 2; else n = 0 (ch untouched). The dedicated LTC output shares wire slots 16–17 with S/PDIF, which get clobbered. |
| `SetVideoSyncMode(any, master, smpte)` | @0x1cc50 | `+0x75 = any<<4 \| master<<5 \| smpte<<6`, ORed into mk3 state byte 14 |

`Drv::UpdateSMPTEVideoSyncState` @0xb6c0 (after a source change, defaults, …), with `ws = Drv::GetWordSource()` @0xb640:
for each box with a video input it calls `SetVideoSyncMode(ws ∈ 4..7, ws == 4+w, ws == 3 && Source ≠ 0 && sourceWire == w)`.

---

## 7. Recipes for a Linux driver

### (a) Lock the card clock to LTC on input channel `ch` of wire `w`

1. Base rate and multiplier already programmed (cb+0x98, cb+0x80, …; [INTERFACES.md](INTERFACES.md) §5).
2. Frame parameters (§3.2): write cb+0x9c, +0xa4, +0xa0, +0xb0, +0xb4; read cb+0x44 → write +0xb8;
   write +0xbc, +0xc0; read cb+0x48 → write +0xe0, +0xe4, +0xf0; cb+0x114 = 1.
3. Freewheel (§3.3): cb+0xc4 = clock_ms·R/1000, cb+0xc8 = addr_ms·R/1000 (or 0xffffffff for infinite;
   the kext default is 0).
4. Word source ws = 3: write cb+0x80 = alt | 0x201 (| 0xF0000 once settled), and cb+0x88/0x8c/0x90 =
   1/0x80000/0 ([INTERFACES.md](INTERFACES.md) §5.2 C/D).
5. Address source: cb+0x84 = 2 (3 for a 2408mk3 dedicated input, with cb+0xa8 = 25w+25),
   cb+0xa8 = 25w+1+ch, cb+0x114 = 1.
6. 2408mk3 on that wire: state byte 14 bit 0x40 (`SetVideoSyncMode(…, smpte = 1)`).
7. Once per buffer: read cb+0x34 (and cb+0x38 for video modes) → §4.2. Read and clear cb+0x40 → detected
   24/25/29/30. If it does not match the configured format, re-run step 2.

### (b) Generate LTC on output channel `ch` of wire `w`

1. Frame parameters as in (a) step 2. These are needed even without a reader (uncertain, but the kext always writes them).
2. cb+0x118 = 1 (stop any running generator).
3. cb+0xcc = X = 25w+1+ch, cb+0xd0 = X (X+12 at 4x), cb+0xd4 = 0. For a 2408mk3 dedicated output:
   cb+0xcc = cb+0xd0 = 0, cb+0xd4 = w+1, plus box byte 15 bit 0x02.
4. cb+0xdc = level (0x7fff default).
5. cb+0xd8 = (X<<16)|X (dedicated with S/PDIF: (X'<<16)|(X'+1), X' = 25w+17), cb+0x11c = 0.
6. Start: choose start sample time t (actual rate), cb+0xe8 = t·a/(100·SR)+1, cb+0xec = Q16 offset
   (§3.4; halve at M = 4).
7. Each buffer: cb+0x4c = current generated frame; cb+0x11c ≠ 0 → the generator overwrote audio (clear to 0).
8. Stop: cb+0x118 = 1.

---

## 8. Uncertainties

* The meaning of TransportState values 3/4/5/6 and of cb+0x34/0x38 states (no strings; §4.2).
* cb+0x44 and cb+0x48 are DSP-provided constants that the kext only reads. The meaning of +0xb8/+0xe0/+0xe4/+0xf0 is inferred from the formula shape only.
* How the DSP treats cb+0x118 (stop) relative to a new start. The kext never clears it, so the DSP
  presumably acks it, or a cb+0xe8/0xec write restarts the generator.
* Whether the cb+0xe8/0xec writes need a commit (the kext writes none).
* Generator mode names (0/1/2) and the exact role of `now` (vtbl 0x2d8 timestamp) in mode 1.
* `GetActualSMPTEDestination`'s remap through Drv+0x10c8 (assumed to be the physical wire slot of the output).
* Output level scale (linear 0..0xffff assumed; 0x7fff default).
* Freewheel units: ms is inferred from ×R/1000. Freewheel is scaled by the base rate only, and the kext default is 0.
* Whether ws 4..7 (video) alone, without a source, still needs cb+0x84 = 0. It does: SendAddressSource yields 0 when ND+0x278 = 0.

---

## 9. Linux implementation

`linux/snd-motu424/motu424_smpte.c` follows the recipes in §7. The controls are listed in the driver README ("SMPTE timecode, video sync and S/MUX").

* **Boot** (`motu424_smpte_hw_init`, from `motu424_clock_init`): frame parameters (§3.2), freewheel (§3.3), generator stopped with no destination, address source none.
* **Settings** are kept as in `Drv` (`SetDefaultSMPTESettings(true)`): 30 fps, follow on, level 0x7fff, freewheel 0, generator off. They survive a reboot of the DSP but not a module reload.
* **Source/destination codes** are those of §2. Validity follows `RebuildSMPTESourceAndDestinationLists`: the channel must be available in the box's current layout and multiplier, a source must be analog (`IsInputAnalog`), and the jack codes need a 2408mk3. After a box, layout, rate or clock change (`motu424_smpte_refresh`, from the Q-Mix layout work, `Card Clock Source` and rate changes), a code that is no longer valid is programmed as 0. It comes back when the code becomes valid again. A box in the middle of a state exchange is judged only after it confirms (the next 100 ms poll).
* **Address source** (§3.1) is rewritten on every card clock change and source change.
* **Generator** (§3.4, §5): mode 0 only (start from the start time as soon as it is switched on). Changing the destination, format or rate stops it and restarts it from the start time. The clobber latch (`cb+0x11c`) is reported once per destination.
* **Polling**: a 100 ms delayed work reads `cb+0x34`/`0x38` (state, §4.2), `cb+0x40` (detected format, cleared after reading; §4.3 with follow), `cb+0x4c` while generating and `cb+0x11c`. It is used instead of the kext's per-interrupt polling.
* **Boxes** (§6): the 2408mk3 byte 14 video/master/LTC bits and byte 15 SMPTE-output bit are ORed into its state message. A box whose previous state is still being sent or confirmed is updated by a later poll.
* **S/MUX** ([INTERFACES.md](INTERFACES.md) §2.3): masks are written after a boot and whenever one changes.

Added assumptions beyond §8:

* `cb+0x4c` is an absolute frame count in the current format. The generator time control converts it to h:m:s:f.
* At 4x the second destination register is X+12. This is the kext's value; its meaning is not known.
* The kext's `f0` write divides by `V·R/64e6`. The driver skips that write if the quotient is 0, instead of dividing by zero.

