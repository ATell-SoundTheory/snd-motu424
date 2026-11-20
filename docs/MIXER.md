# MOTU PCI-424 DSP mixer: program generator and download

This document covers the output mixer of the MOTU PCI-424 family as implemented by the
macOS kext (x86_64 slice). The kext does not configure a fixed-function mixer. Instead, the host
**generates TI C62x machine code** describing the whole output mix and downloads it into
one of two program banks in DSP memory. The DSP firmware then switches to that bank with a
short crossfade.

The C port is `linux/snd-motu424/motu424_mixer.{c,h}`. Addresses in this document such as `@0x24490` refer to
functions in the kext binary.

Names containing `VendorMixer` are descriptive aliases for vendor symbols or
properties, not their literal names.

**Verification.** The original `MixerState` constructor and mutators, plus `GenerateMixer`, were run
under an x86-64 emulator (unicorn), with only `IOMalloc`, `memcpy` and `IOFree` stubbed. The C port
produced bit-identical programs in the following cases:
- default states with 0, 1, 3, 8, 12, 24 and 48 DMA pairs;
- the routing, fade, metering and split scenarios in `tools/mixer_test.c -x`;
- 480 random mutation sequences.

The download path was ported by reading the code only; it was not emulated.
The userspace harness now asserts both download paths, write counts, unchanged banks
after DMA failure, and invalid-argument/buffer-bound handling. The kernel integration
adds a DMA write barrier before starting a transfer and logs routing-fade timeouts.
The bank is committed only after a successful download and switch request.

## Data structures

| Original | Size | Port | Notes |
|---|---|---|---|
| `MixerState` | 0x1c570 | `struct motu424_mixer_state` | 48 `MixOutput` @+0, 180 `MixRouting` @+0x10080, instruction tail @+0x1c560, metered output @+0x1c568 (-1) |
| `MixOutput` | 0x558 | `struct motu424_mix_output` | one per **stereo output pair** (96 channels = 4 interfaces x 24) |
| `MixRouting` | 0x118 | `struct motu424_mix_routing` | one per **DSP fader** (180) |
| `Instruction` + 13 subclasses | 0x48-0x58 | `struct m424_ins` (private) | embedded in outputs and routings; the port keeps them in a separate work area |

`MixOutput` fields:
- `altType` (+0): 0 = stereo, 1 = split, 2 = no code.
- `dmaEnabled` (+4): host playback feeds this pair.
- `nrout` (+8).
- First routing (+0x10).
- `metered` (+0x18).
- `storeInc[2]` (+0x19/+0x1a). This is 2, or 3 for the last pair of each 12-pair (24-channel) interface block; the extra word skips one slot per interface.

`MixRouting` fields:
- `state` (+0): 0 = off, 1 = on, 2 = fading in, 3 = fading out.
- `flag` (+4): a bool passed through `Drv424::ConnectFader`. Its meaning is unknown. It only changes metering order.
- Output (+8) and doubly-linked list links (+0x10/+0x18). New routings are pushed at the **head** of the output's list.

The routing index is also the fader index. Its `LoadInput` reads the fader's
post-gain sample from `*±A12/A13/A14[k]` (B side: B12-B14):
- 0..62: A12, k = i-31
- 63..125: A13, k = i-94
- 126..179: A14, k = i-157

`SetupRouting` and the default state:
- `Drv424::SetupRouting(bool)` @0x18770 walks the DMA output channels in pairs. For each pair it calls `SetAlternateChannelType(hw/2, type)` and `EnableDMA(hw/2, active)`.
- `type` is 0 when the next channel's hardware index is `hw+1`. Otherwise it is 1 if the channel descriptor's byte at +0xc is set, else 2.
- `DownloadNewMixer` @0x161f0 regenerates into a 0x2800-byte buffer and downloads only if something changed.
- With the identity channel table, every pair is type 0. `RestoreDefaultVendorMixer*` connects no faders.
- Therefore the default playback program is: for every pair, load the 2x2 DMA words, align (shift), OR-combine and store. No faders or gain are involved, so the result is unity gain and DMA pair N goes to output pair N. `motu424_mixer_init_default()` builds exactly this state.
- At 1x the channel records are indexed by hardware channel, so every pair is type 0 and `EnableDMA` follows the first channel of the pair. Outputs are therefore always handled as stereo pairs. DMA-fed pairs take their two playback slots in ascending pair order; pairs without DMA store silence.
- `Drv::StartAudioRunning` @0x13a80 calls `SetupRouting(true)` first, while the engine is still stopped, and `Drv::StartIdling` @0x139c0 calls `SetupRouting(false)`. A new program is thus downloaded before the interrupts are enabled.

**Hardware-verified** (PCIe-424, 48 kHz):
- The DSP idles without any program having been downloaded, as it does under the kext while no box is known.
- Programs with gaps work: with 24, 24, 2 and 24 outputs on the four ports (pairs 0–23, 24 and 36–47 DMA-fed, 679 words), a tone played into the first slot after the gap arrives on the first output of the fourth box.
- Downloading again with the engine stopped works (37 → 25 → 37 pairs after a box was switched off and on). The routing-fade wait never timed out.

## Instruction graph (`MixOutput::ConfigureInstructions` @0x239e0)

The 48 outputs are configured in order. Two values are threaded from one output to the next:
- `depIns`: the previous load or zero, used to serialise pointer bumps.
- `store`: the previous store, used to keep the stores in order.

1. **Load or zero** (`ConfigureLoadOrZero` @0x23200):
   - Not DMA-enabled: `Zero` (SUB r,r,r), using 1 register, or 2 if `nrout>1 || metered || alt==1`.
   - DMA-enabled:
     - `LoadOut A` (2 registers): `LDW *+A1[0]` and `*+B1[1]`.
     - `LoadOut B`: `*+A1[1]` and `*+B1[0]`.
     - Two `Shift` instructions, then `Combine` (OR across sides).
     - `Bump`: `ADD A1+=A0` and `B1+=A0`, which advances the DMA pointers.
     - Dependency latencies: load→shift 5, shift→combine 1, LA→LB 0, LB→bump 0.
2. **Routings** (`MixRouting::ConfigureInstructions` @0x23750), in list order:
   - `LoadInput` (1 register).
   - If the routing is fading: `Fade` (LI→Fade latency 5).
   - `Add` into the accumulator. Latency is LI→Add 5, or Fade→Add 2, plus 1 from the previous Add.
   - The `Add` variant depends on the routing:
     - Single routing: `SADD`.
     - Multiple routings, first one: `ADD int+int→long`.
     - Multiple routings, later ones: `ADD int+long→long`.
3. **Metering** (`DoMetering` @0x238d0) applies only to the output selected by `MeterOutput`:
   1. Routings with `flag` set are added first.
   2. Then `ShiftRight 8` (long or int), `MeterStore` to `*+A14[23]`/`*+B14[23]`, and `ShiftLeft 8`.
   3. Then the remaining routings, followed by the same sequence storing to `[24]`.
4. **Saturate** (`SAT` long→int) if `nrout>1 || metered`.
5. **Store**:
   - Stereo: one `Store`, `STW A_r,*A15++[inc0]` and `STW B_r,*B15++[inc0]`.
   - Split (alt 1):
     - `Zero` of register r+1 (offset 1).
     - `Store1`: `*A15++[2]` stores A_r; `*B15++[2]` stores B_{r+1} from the A file (side 1).
     - `Store2`: `*++[inc1]`.
     - The stores are chained.

   Stores with a negative `nregs` free the accumulator registers.

`Instruction::Reset` @0x20520 does not clear the resource mask or the subclass fields. The port
re-creates instruction objects on every generate. This is equivalent, as the emulator diff confirms.

## Scheduler (`Scheduler` @0x22480, `ScheduleInstructions` @0x22850)

Instructions form a singly-linked list in append order behind a dummy head. Each `ExecutePacket` holds instructions. Every
instruction emits two words: the A-side half (`.?1`) and the B-side half (`.?2`). Both halves process the left and right channels in parallel.

- **Resources.** The low nibble is the functional unit: 1 = L, 2 = S, 4 = D, 8 = M. Higher bits mark cross-path or long use. Two instructions sharing any resource bit cannot be in the same packet.

  | Instruction | Mask |
  |---|---|
  | LoadOutput | 4 |
  | Shift | 2 |
  | Combine | 0xc1 |
  | Zero | 2 |
  | Bump | 0x81 |
  | LoadInput | 4 |
  | Fade | 0x48 |
  | Add | 1 (SADD), 0x21 (first) or 0x31 |
  | Saturate | 0x11 |
  | Store | 0x14 |
  | ShiftRight | 0x32 (long) or 2 |
  | ShiftLeft | 0x22 |
  | MeterStore | 0x14 |
- **Registers.**
  - The free mask starts at 0x7fc, i.e. registers 2..10.
  - Single registers are allocated from 10 downward.
  - Pairs are allocated at even indexes from 0 upward (so 2, 4, 6 and 8).
  - Allocation fails if nothing is free.
- **Per cycle,** the scheduler scans the list:
  - Skip an instruction if any of these holds:
    - its resources are busy;
    - its dependencies are not met (`dep.cycle + latency <= cycle`);
    - it is not the list head and not a D-unit instruction, and it wants registers that would push usage above 6;
    - register allocation fails.
  - The scan stops after more than 20 skips.
  - When an instruction is scheduled, its dependency list is emptied. The instruction is unlinked from the list.
  - A packet is finished early in either case:
    - all four units are used;
    - a D unit is used, the packet exactly fills the rest of the 32-byte fetch packet, and one of the next 8 D-unit instructions could issue next cycle.
- **Fetch-packet padding.** If the new packet does not fit in the remaining words of the current 8-word fetch packet, the **previous** packet is padded with NOP words (value 1, p-bit set). For this reason packets are emitted one step late. An empty cycle becomes a single NOP.
- **Emission.** Each packet emits `ins A, ins B, ..., nops`, and the p-bit of the last word is cleared.
- **Termination.** The original stops when the list is empty or more than 0x9f4 words have been emitted, silently truncating an unfinished program. The port returns `-ENOSPC` instead if instructions remain. Program size depends on routing fades, DMA, metering and split outputs.
- **Epilogue.** The program ends with `0x001c00e2, 0x079803e2, 0x00008000`. The last word is NOP 5. The first two are an S2-unit branch and an MVC-type instruction, probably a return to the firmware's interrupt loop.
- **Return value.** `GenerateMixer` returns the word count.

### Opcode templates

Notation:
- `r` = source instruction's register; `v<<23` is the dst field, `<<18` is src2/baseR, `<<13` is src1/offset.
- Bit 1 = side (s), bit 0 = parallel (p).
- The A word ends in `...1` and the B word in `...3`, unless the template shows otherwise.

| Instruction | A word | B word | Meaning (C62x decode) |
|---|---|---|---|
| LoadOutput t0 | `reg<<23 \| 0x40265` | `(reg+1)<<23 \| 0x422e7` | LDW *+A1[0],A_r / LDW *+B1[1],B_r+1 |
| LoadOutput t1 | `r<<23 \| 0x42267` | `(r+1)<<23 \| 0x402e5` | LDW *+A1[1],B_r / LDW *+B1[0],A_r+1 |
| Shift t | `v<<23\|v<<18\|0x16ce1`, v=r+(t==1) | `v<<23\|v<<18\|0x169e3`, v=r+(t==0) | S-unit shifts, src1 field 11 |
| Combine | `r,r,r \| 0x1ff9` | `r, r+1, r+1 \| 0x1ffb` | OR .L1X / .L2X |
| Bump | `0x00802079` | `0x0080307b` | ADD .L1 A0,A1,A1 / ADD .L2X A0,B1,B1 |
| Zero | `v,v,v \| 0x5e1` | `\| 0x5e3` | SUB .S v,v,v; v = (own or src reg) + offset |
| LoadInput | `base<<18\|reg<<23\|abs(k)<<13\|(k>=0)<<9\|0x65` | `...\|0xe7` | LDW *±A12..14[abs(k)] |
| Fade in | `0x20000000\|r<<23\|r<<13\|0x1181` | `...\|0x183` | [B0] M-unit multiply by ramp, x-path |
| Fade out | `r<<23\|r<<13\|0x1581` | `...\|0x583` | M-unit multiply, other half of ramp |
| Add | `a<<23\|a<<18\|b<<13\|op\|1`, op = 0x278 / 0x478 / 0x438 | `...\|3` | SADD / ADD i+i→l / ADD i+l→l |
| Saturate | `r<<23\|r<<18\|0x819` | `\|0x81b` | SAT .L |
| ShiftRight | `r<<23\|r<<18\|0x10000\|(long?0xd20:0xda0)\|1` | `\|3` | SHR by 8 (long or int) |
| ShiftLeft | `r<<23\|r<<18\|0x104a1` | `\|0x104a3` | SHL by 8 |
| MeterStore | `r<<23\|0x380000\|abs(k)<<13\|(k>=0)<<9\|0x75` | `...\|0xf7` | STW *+A14[k] / *+B14[k], k = 23 or 24 |
| Store | `R<<23\|inc<<13\|0x3c1675\|(sideA==1)<<1` | `(R+c)<<23\|inc<<13\|0x3c16f5\|(sideB==1)<<1` | STW *A15++[inc] / *B15++[inc] |

Register usage implied by the firmware ABI:
- A0: DMA stride.
- A1/B1: DMA output pointers.
- B0: fade ramp. It is fed by `cb+0x70` = `step<<16 | (u16)-step`.
- A12-A14 and B12-B14: fader output buffers.
- A15/B15: output store pointers.
- A11/B11: probably a shift count.

## Download protocol (`ND424::DownloadMixer` @0x2c000)

All addresses are DSP addresses. `ND424::DSP::Write`/`Read` (@0x29d80/@0x29df0) map them as follows:
- If `addr>>23 == 3`, the access goes to the register window at `addr & 0x7fffff`. **Reads** from this window are byte-swapped; writes are not.
- Otherwise the access goes to the memory window at `addr & 0x3fffff`.
- `WritePolitely` is a write followed by `IODelay(1)`.

`cb` (ND+0xb8) is the control-block address. At boot the kext obtains it as follows:
1. Write 0 to the firmware-descriptor address at +0x44.
2. Interrupt the DSP.
3. Poll that address until it is non-zero, with a 250 ms timeout.

Next, `ND424::StartDSP` (@0x2d1a0, code near 0x2d32c) caches the addresses of the tables listed below and sets bank = 0. It then clears the
180-byte fader-input table and writes it in full.

| cb offset | Use |
|---|---|
| +0x00 | fader-input table address (ND+0xbc) |
| +0x04 | fader gain table (ND+0xc0), 8 bytes per fader (L, R) |
| +0x08 | fader fade-increment table (ND+0xc4) |
| +0x0c / +0x10 | DSP address of mixer program bank 0 / 1 |
| +0x14 | input routing table (ND+0xc8) |
| +0x18 | trim table (ND+0xcc) |
| +0x70 | routing-crossfade ramp step `step<<16 \| (u16)-step` |
| +0x74 | DMA go (1) / idle (0) |
| +0x78 | address of the new mixer program |
| +0xf8 / +0xfc / +0x100 | DMA host bus address / DSP destination / byte count (DSP clears the count when done) |
| +0x104 | "switch to new mixer" (write 1) |
| +0x108 | routing fade done (DSP sets non-zero; host clears it) |
| +0x10c | volume fade counter (host writes 0xf0 to start; waits for 0) |
| +0x110 | trim update request (0x100), waits for 0 |

The download sequence:
1. `bank ^= 1`; `addr = R(cb + 0x0c + 4*bank)`.
2. Copy the program to `addr`:
   - **PCI-424 (`ND424::DownloadMixerGuts` @0x2b5c0, C6205):** split the program into chunks of at most 0x400 words. For each chunk:
     1. Copy it to a 4 KiB host DMA buffer that is below 4 GB (`AllocateMixerDownloadBuffer` @0x2ca30).
     2. `W(cb+0xf8, bus)`, `W(cb+0xfc, addr)`, `W(cb+0x100, bytes)`, `W(cb+0x74, 1)`.
     3. Poll `R(cb+0x100) != 0` every 20 µs, at most 10000 times.
     4. `W(cb+0x74, 0)`.
     5. If the transfer is still busy, give up silently; the bank is not switched. Otherwise `addr += 0x1000` and continue with the next chunk.
   - **424X / PCIe (`ND424b::DownloadMixerGuts` @0x2dfc0):** `WritePolitely(addr + 4*i, word[i])` for every word.
3. `WaitForRoutingFadeToComplete` @0x2b540: poll until `R(cb+0x108) != 0`, in 5 µs steps, with a 50 ms limit. The result is ignored.
4. `step = 0x10000 / (rateMult * 480)` (a 480-sample crossfade; rateMult is 1, 2 or 4).
5. Write each of the following with `WritePolitely`:
   - `cb+0x108 = 0`
   - `cb+0x78 = addr`
   - `cb+0x70 = step<<16 | (u16)-step`
   - `cb+0x104 = 1`

Callers (for example `Drv424::UpdateVendorMixerRouting` @0x18cd0) handle routing changes in this order:
1. `WriteFaderInputTable`.
2. `DownloadNewMixer`. Routings that are fading in or out produce `Fade` instructions, so the switch is a crossfade.
3. `MixerState::CompleteFades`.
4. Often `WaitForRoutingFadeToComplete`, followed by another download that no longer contains the fades.

### Faders

These are related functions; the port provides helpers for them.
- `ComputeLeftAndRightGain` @0x2a760:
  - `g = ((u64)(a*b))^2 >> 45`
  - `L = ((0x4000 - p²)·g >> 14) << 16`
  - `R = ((256p - p²)·g >> 14) << 16`
  - Pan p ranges over 0..0x80, with 0x40 as centre. With a = b = 0x8000 at centre this gives 0x60000000 on each side.
- `SetFaderImmediately` @0x2c440 writes L and R to `gainTbl + 8f` and `+4`.
- `SetFaderInput(f, hw)` @0x2db50 sets byte `[f^3]` of the 180-byte table to `hw + hw/24`. The `^3` gives big-endian packing within each dword. The function tracks a dirty dword range.
- `WriteFaderInputTable` @0x2c7f0 writes the dirty dwords to `inputTbl + 4k` and resets the low end of the range to -1.
- `SetChannelVolume` and `SetFaderWithCrossfade`:
  - They compute a per-sample increment `(new>>10 - old>>10)/240` and write it to `incTbl + 4f`, after pre-adjusting the gains for the rounding remainder.
  - They then start the fade with `cb+0x10c = 0xf0` (240 samples) and wait for it to reach 0 (`WaitForVolumeFadeToComplete` @0x2ba50, 50 ms).
  - `SetChannelVolume` indexes the channel as `ch + ch/24`.
- `SetTrimFactors` @0x2c4c0:
  1. Compare 100 requested 8-bit factors with the cached factors, initially
     `0x40`. If they are unchanged, do nothing.
  2. Wait for `cb+0x110 == 0`; the kext proceeds if its deadline expires.
  3. Write 50 dwords to `trimTbl`, each containing two signed 16-bit
     differences between requested and cached factors, with the first factor
     in the high halfword.
  4. Write `cb+0x110 = 0x100` and cache the requested factors. The kext
     returns without waiting for this new update to complete.

  The original routine was emulated with DSP reads and writes stubbed for
  100 random old/new factor vectors and one unchanged vector. Its packed
  deltas, request and cache matched this protocol; the DSP itself was not
  emulated.

  `Drv424::UpdateBoostState` @0x177a0 maps hardware input `i` to factor
  `i + i/24`, leaving four padding factors, and uses zero for muted inputs.
  The Linux port waits for an earlier update before overwriting the table and
  fails with `ETIMEDOUT` if it remains busy. At 4x it mirrors an available
  input's factor to its partner 12 slots later in the same port. At 48 kHz,
  changing trim from 64 to 128 increased captured and monitored noise on one
  24i input by about 6 dB; disabling its switch silenced both paths.

`ND424::SwapLevelMeters` @0x2caf0 alternates and clears 52-value DSP meter
buffers and can also alternate a 48-entry input-selection table. It signals
the selected banks through `cb+0x7c`, polls `cb+0x30`, and reads the previous
meter buffer. `cb+0x1c/+0x20` point to 104-byte peak banks;
`cb+0x24/+0x28` point to 48-byte input-selection banks. Each exchange waits
for `cb+0x30` to clear, optionally flips/writes the input bank, flips/clears
the new peak bank, writes peak-bank bit 0 and input-bank bit 1 to `cb+0x7c`,
waits again, then reads the retired peak bank. Input selectors use
`i + i/24`, packed high-byte-first; peaks are high-halfword-first.

`LevelMeters::ReadMeters` @0x25410 manages the requested inputs and peak
decay; this is not a direct read of all 96 inputs. The Linux controls select
one 48-input bank and an optional output pair, and return 52 raw values:
48 inputs, then four values for the metered output pair (see below). Selection
changes prime the DSP before the first reported snapshot. The vendor
application's software trim/gain correction and square-root display
compression are not applied. A post-request timeout faults meter
reads until reload because ownership of the banks is then uncertain.

### Peak value format

Read from the PCI-424X/PCIe-424 DSP image (`pci424b-dsp.bin`, loaded at 0; the
data page register B14 is 0x6e70 and the control block is `B14 + 0x13c`):

- **Inputs** (routine 0x64c0–0x65e8, 24 passes of two inputs).
  Each selector byte indexes the current input frame, which holds one 32-bit
  word per input with the 24-bit sample in the low bits. The routine computes
  `SHL 8`, `ABS`, `SHRU 16`, takes the larger of that and the stored 16-bit
  peak, and stores the pair as one word (first input in the high half). So
  **raw = |24-bit sample| >> 8**, range 0..32767, and
  **dBFS = 20·log10(raw / 32768)**. One raw step is −90.3 dBFS.
- **Before trim.** The caller (0x2ba8) runs this routine on the frame and only
  then calls the trim routine at 0x62c0, which multiplies the same frame in
  place by the factors (`>> 6`, so 64 is unity). Input peaks therefore do not
  include the trim factor or the input switch.
- **Outputs** (0x2a94–0x2bf4). The mixer program's two `MeterStore` words per
  side sit behind the 180 fader outputs (words 180 and 181 of the tables at
  `B14 + 0xe50` and `B14 + 0x1138`, i.e. `*+A14[23]`/`[24]` and the B-side
  equivalents). Each value goes through `ABS`, `SSHL 8`, `+0x80`, `SHRU 16`,
  the same scale as the inputs because the program stores `accumulator >> 8`.
  The four slots are, in order: `|[24] − [23]|` left and right, then `|[24]|`
  left and right. With the Linux driver, which connects every mixer route
  with `flag = false`, `[23]` is the host playback alone and `[24]` adds the
  monitor routes. Slots 48/49 are therefore the monitor mix without host
  playback, and slots 50/51 the complete output before saturation.

The PCI-424 image (`pci424-dsp-prog.bin`) uses the same `<< 8`, `ABS`, `>> 16`
sequence (around 0x7968 and 0x5aa8) but was not traced in full.

Checked on a PCIe-424 at 48 kHz with idle analog inputs: the maximum of the raw
input peaks over a 3.5 s capture equalled `max |s32| >> 16` of the simultaneous
ALSA capture on 91 of 96 inputs (values 1–15; the other five captures peaked
within 32 LSB of a step). Raising one input's trim from 64 to 128 doubled its
captured peak and left its meter value unchanged. The output slots were
derived from the code only; no playback measurement was made.

## MOTU's macOS mixer control path

The Windows installer includes a PCI-424-specific TouchOSC layout, and the macOS
installer's shared `Common.pkg` includes MOTU's macOS mixer. The mixer executable has a
`Device424` class and imports IOKit connection calls; the PCI-specific "MOTU PCI Audio
Setup" app is a separate application. This establishes that MOTU's macOS mixer has a PCI-424
device path, but it does not mean the Mac application can control this Linux driver.

The bundled mixer executable formats legacy fader values through
`DigiVolToDecibelValue`, which computes `40 * log10(value / 32768)`; this matches the
Linux DSP volume curve, with zero representing silence (`−∞ dB`). Its legacy meter
update path forwards reconstructed raw peak values to the meter subsystem; the
calibration of those values comes from the DSP firmware (see "Peak value format"
above), not from the application. The binary
also contains `Set Pad`/`kInputPad` and `Set Phantom`/`kPhantom` controls. MOTU's macOS mixer
is shared with MOTU's FireWire/USB interfaces, which have mic preamps. The PCI kext,
HAL plug-in and PCI Audio Setup have no phantom, pad or preamp-gain strings or
properties, so these controls do not apply to PCI-424 interfaces. Their only analog
setting is the switchable input reference level on the 24I/O and 2408mk3
(INTERFACES.md sections 2.3 and 2.4).

The kext's route-creation path is `Drv424::ConnectFader` @0x16410:
1. Take a fader index from the driver's available-fader queue.
2. Resolve a mixer channel record to its hardware input, and write its
   bus/route gain and pan to the fader gain table.
3. Write the hardware input to the fader-input table.
4. Link that fader to a stereo output pair in `MixerState`.

At 44.1/48 kHz, MOTU's driver provides 180 fader slots and 48 stereo output pairs.
One fader normally represents one active input-to-output-pair route. The driver
constructs 180 routing objects and writes a 180-entry DSP fader-input table; this is
not an ALSA or PCI limit. Whether different DSP firmware could support more is unknown.
`UpdateVendorMixerRouting` writes changed fader inputs and downloads a new whole mixer
program; as described above, the DSP transitions routing changes over 480 samples.
Volume changes go through `SetFaderWithCrossfade` and use the separate 240-sample gain
ramp.

`UpdateVendorMixerVolumes` waits for any current 240-sample ramp, walks the channel and
sequencer state, and applies changed fader values through `SetFaderWithCrossfade`.
It starts another ramp only if one or more fader writes changed the DSP tables.

### App-to-kext live updates

The PCI-specific `HALPlugin.bundle` inside `MOTUPCIAudio.kext` supplies the missing
interface between MOTU's macOS mixer and the kernel. In the 32-bit binaries,
`CoreDeviceAW` obtains `VendorMixerAPIImpl` through `AudioWireCardImpl::GetVendorMixerAPI`.
The app's volume, pan and mute calls use API vtable offsets `+0x44`, `+0x48` and
`+0x40`; bus volume and input mapping use `+0x50` and `+0x6c`.

`VendorMixerAPIImpl` maps two driver-owned settings buffers with `IOConnectMapMemory`:
type 2 is the regular buffer and type 3 is the stereo buffer. The kext's
`UC::clientMemoryForType` returns the corresponding pointers and lengths. The
HAL setters modify those shared buffers directly and accumulate action bits; the
matrix values are **not** serialized in the live property dictionary.

On commit, `CoreDeviceAW::CommitChanges` calls `AudioWireCardImpl::CommitChanges`,
which calls `VendorMixerAPIImpl::AddPendingMods`. For ordinary settings, the HAL sends
the equivalent of the following Core Foundation dictionary with
`IOConnectSetCFProperties`:

```text
{"Set": [{"Property": "Action", "Value": action_mask}]}
```

A synchronous commit can also include a `Sync` entry. The kext's `setProperties`
passes `Set` to `Engine::SetPropertiesFromPlugIn`, which dispatches the `Action`
bits to the driver:

| Mask | Kext action |
|---:|---|
| `0x01` | `UpdateVendorMixerRouting`, followed by a mixer property refresh |
| `0x02` | `UpdateVendorMixerVolumes` |
| `0x04` | `UpdateBoostState` for input trim/mute |
| `0x08` | Clear clipping indicators |

Stereo-settings setters also accumulate `0x10`. The x86-64 kext's `Action`
dispatcher checks the four bits above but has no corresponding `0x10` handler.
The application translates stereo adjustments into the ordinary volume/pan
writes described below; stereo metadata does not require a separate DSP mode.

### Settings buffers

The kext exports two persistent settings buffers:

| Property | Size | Observed organization |
|---|---:|---|
| `VendorMixerSettings` | `0x4980` | `0x180`-byte prefix, then 48 × 96 four-byte cells |
| `VendorMixerSettingsStereo` | `0x3780` | Three `0x1200`-byte planes, then 96 four-byte input mappings |

`Drv424::GetVendorMixerSettingsBuffer` and `GetVendorMixerStereoSettingsBuffer` return those sizes.
`CreateVendorMixerProperties` also publishes `LastFaderBus`, `LastFaderInput`, `VendorMixerFaders`,
`SequencerFaders`, `MaxFaders`, and `FadersPerBus`.

MOTU's macOS mixer accepts bus IDs 0 through 47 and converts each to the even output-channel
ID `2 * busID`. The regular cell for a bus and input is therefore at
`0x180 + 4 * (busID * 96 + inputChannel)`. The HAL setters and kext fader
allocation identify its fields:

| Cell offset | Meaning | Action |
|---|---|---:|
| `+0` | 16-bit route volume, `0..0x8000` | `0x02` |
| `+2` | Pan, `0..0x80` | `0x02` |
| `+3`, bit 0 | Solo | `0x01` |
| `+3`, bit 1 | Mute | `0x01` |

For regular routes, mute vetoes the route. If a bus has an enabled solo-marked
input, only solo-marked inputs are selected; otherwise an unmuted input can be
selected even when its solo bit is clear. Bus mute, channel/output eligibility,
and talkback/listenback also affect routing. In particular, bit 0 is not a
general route-enable bit.

The regular prefix contains:

| Offset | Meaning | Action |
|---|---|---:|
| `2 * inputChannel` | Input trim byte (`0x40..0xff`) and input-mute byte | `0x04` |
| `0x0c0 + 4 * busID` | 16-bit bus volume (`0..0x8000`) and bus-mute byte at `+2` | `0x02` / `0x01` |

`UpdateBoostState` uses zero trim for a muted input. `MaybeAllocateFader` passes
the bus volume, route volume and route pan to `SetFaderImmediately`. Defaults
are input trim `0x40`, route volume `0x8000`, pan `0x40`, route mute set,
bus volume `0x8000`, and bus mute set.

Separately from the settings flags, the route allocator tracks each cell with two
low bits: bit 0 records whether its fader is currently allocated, and bit 1 records
the desired state. `UpdateVendorMixerRouting` updates the desired bit, then
`MaybeFreeFader` disconnects state `01` and `MaybeAllocateFader` connects state `10`.

The stereo buffer has one byte per bus/input in each plane: balance at `0`,
width at `0x1200`, and a balance/width preference bit at `0x2400`. Its tail at
`0x3600 + 4 * inputChannel` stores the input mapping. The initializers fill the
planes with `0x40`, `0x40`, and `0`, and zero the tail. MOTU's macOS mixer examines the
96 mappings when forming adjacent stereo pairs. In the i386 application,
`CoreDeviceAW::ApplyDisplayedPan` @0x32bd70 translates a linked pair's displayed
balance or width into ordinary route pan and volume writes:
- For balance `b` in 0..128, it takes the greater of the two route volumes as
  `v`, pans the routes fully left/right, and multiplies the left volume when
  `b > 64` or the right volume when `b < 64` by `b * (128 - b) / 4096`.
- For width `w`, it sets both route volumes to their greater prior value and
  pans them to `w` and `128 - w`.

The app also updates the stereo metadata. The Linux balance/width controls
apply these transforms atomically to adjacent physical inputs, overwriting
their mono volume/pan rather than retaining hidden mono settings. Readback is
the last requested stereo metadata; mono edits remain possible and rewriting
the same stereo value reapplies the transform. Persistent stereo links and
coupled mute/solo behavior are not implemented by those ALSA controls.

The web UI implements its persistent links separately and does not use these
destructive stereo controls. Its gain/balance describe the two stored route
volumes, while pan is their pan midpoint and width is
`64 + (rightPan - leftPan) / 2`. Thus the web width runs from reversed on the
left to normal stereo on the right, opposite to the driver's width code.
Half-step midpoint/width coordinates preserve every integer mono-pan pair.
Linking does not project the pans onto a mirrored pair. Stereo pan translates
both pans equally, stopping at an edge; width changes separation at a fixed
midpoint. The UI synchronizes shared trim, input enable, mute and solo itself.

### Talkback and listenback

The activation setters (`Drv424` @0xe040/@0xe100) and source/dim accessors
(@0xe180..0xe320) feed the ordinary route allocator and gain update path.
`UpdateVendorMixerRouting` @0x18cd0 (branches @0x18fcc/@0x19060) forces an active
microphone into its selected unmuted buses despite ordinary route mute/solo,
subject to channel availability. The Linux allocator reserves a fader for
these forced routes without double-counting an ordinary enabled route.

`UpdateVendorMixerVolumes` @0x17d60 (branches @0x18529..0x185d9) multiplies the
**route volume** of other inputs on those buses by `dim/128`, before the
nonlinear fader gain calculation. Talkback dim wins for non-talkback inputs
when both functions apply; otherwise listenback dim is considered. This is
not a linear amplitude/dB multiplier and does not dim host PCM. Microphones
retain their normal route volume/pan and global input trim/mute.

When the user-client owner changes, it restores persisted `VendorMixerSettings` and
`VendorMixerSettingsStereo` data after checking their lengths. It then rebuilds routes;
missing or wrong-sized data are replaced with defaults before the route update.
The separate `SequencerVendorMixer` property recognizes `Reset`, `Enable`, `Input`,
`Output`, `Volume`, and `Pan`; it is not the ordinary matrix's live update path.

**Offline verification.** The actual i386 HAL setter code was emulated for all
4,608 regular cells, boundary/error cases, route and bus controls, input controls,
and stereo fields. Its commit code emitted the dictionary above with the combined
action mask; Core Foundation and IOKit calls were stubbed. The x86-64 kext's solo
predicate, mute/solo routing branch, and action callback targets were checked
separately. This does not emulate the complete DSP or establish audible monitoring.

The PCI-424 TouchOSC layout shipped in the Windows installer exposes a broader UI surface:
bus selection and master fade/mute; per-input fader and pan; input mute and mix solo;
meters and PFL indicators; trim, pad, and pre/post controls; and talkback/listenback enable
and balance. This is evidence for the control surface's OSC API, not proof that every
control has been traced to a PCI-424 hardware operation.

The fader helpers now have offline tests for center/left/right pan, zero gain, fader gain
table writes, and the packed fader-input table update. These tests verify the host-side
math and register writes only; they do not prove an audible monitor mix.

### Linux control status

The Linux driver exposes indexed controls for per-input trim and switch, route volume,
pan, switch and solo, plus bus volume and switch, adjacent-pair stereo
balance/width, talkback/listenback and raw DSP peak snapshots. Indices correspond
to physical inputs and output pairs, not the compact PCM layout. Route and bus switches default to off;
bus controls do not affect host PCM playback. The route allocator reserves the 180 slots
conservatively for all unmuted requested routes on unmuted buses, including unavailable
and solo-suppressed routes and forced microphone routes. It rejects excess
requests with `-ENOSPC`. It waits for fades before reusing removed slots and
reports an uncertain DSP state after a failed
transition. See the [driver README](../linux/snd-motu424/README.md) for control names,
indices and safety precautions.

The idle-path trace shows that `StartIdling` calls `Drv424::SetupRouting(false)`. That
routine clears the host capture-routing table, adjusts DMA/alternate-channel state, and
regenerates/downloads the mixer program if that state changed; it does not explicitly
disconnect mixer faders. It then invokes the ND setup with the idle flag. This makes
persistent DSP routes during PCM idle plausible, but does not prove that the DSP still
receives interface inputs or that an input-to-output monitor path is audible in that mode.
The Linux allocator, gain ramps, control mapping and simulated topology transitions have
offline tests. At 48 kHz with capture active, 24i inputs 1 and 14 (control indices 48
and 61) were routed to 24I/O outputs 19 and 20 and measured through loopback. Pan and
solo selected the expected channels; muting either input silenced its corresponding
capture and monitored signal. Subsequent checks on the same pair/bus verified
stereo normal/mono/reversed routing, balance suppression of the left source,
talkback/listenback forced routing and dim-to-zero/restoration, and meter
exchange with nonzero right-channel output values. These checks used active
capture; they do not establish idle audible monitoring or meter calibration.

## Uncertainties

- Exact mnemonics of the `Shift` (src1 field 11), `Fade` (M-unit op 3 or 11, ramp in B0 halves) and epilogue instructions are inferred from the C62x field layout, not from a disassembler. The port emits the same bits, so this does not affect correctness.
- What alt type 1 (split) means at the hardware level, which descriptor property (+0xc) selects it, and the meaning of the routing `flag`.
- The DSP-side semantics of `cb+0x108`. On a PCIe-424 it was already set at the first download after boot and at every later one, so the wait never timed out. If it is not set, the original waits 50 ms and proceeds anyway.
- The download path was checked by reading the code and, for the PIO variant (ND424b), on a PCIe-424. The DMA variant of the PCI-424 has not run on hardware. The `rate_mult` field must match the value at `ND+0x1ec`, set by `SetSampleRateMultiplier`.
- ND424b (424X/PCIe) memory map differences beyond the PIO download were not examined.
