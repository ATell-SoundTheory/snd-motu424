# MOTU PCI-424 family – host interface

This document describes the host-side programming model of the MOTU "AudioWire" PCI cards. It was recovered by static analysis of MOTU's macOS driver (`MOTU Audio Installer 1.6 (73220).pkg`, the x86_64 slice of `MOTUPCIAudio.kext`, which is unstripped) and cross-checked against the Windows driver (`MOTUAW.sys` in `MAI_Win_66814.zip`).

Addresses written as `0x2d1a0` are virtual addresses in the x86_64 kext slice.

**Hardware status:** a PCIe-424 (`137a:0005`) with a 24I/O, a 2408, a 2408mk2 and a 24i has been run with the Linux driver at 48 kHz: boot, FPGA and DSP start, interrupts, the idle engine, interface identification and clock setup of all four boxes, a channel layout that follows the boxes, 96-channel capture, and playback checked through analog loopbacks on the 24I/O, the 24i and the 2408mk2. Everything else in these documents is from static analysis only.

Related documents:
* [INTERFACES.md](INTERFACES.md): breakout interfaces, channel maps, clocking and the status word.
* [MIXER.md](MIXER.md): the DSP mixer program that MOTU's driver generates at runtime.

## 1. Card family

| PCI ID | Product | DSP | FPGA programming | Supported |
|---|---|---|---|---|
| `137a:0001` | PCI-324 | ? | ? | **no** – see §9 |
| `137a:0003` | PCI-424 | TI TMS320C6205 (PCI built in) | Altera through McBSP pins | yes ("424a") |
| `137a:0004` | PCI-424X (PCI-X) | TI TMS320C6412 | Altera through GPIO | yes ("424b") |
| `137a:0005` | PCIe-424 | TI TMS320C6412 behind a TI XIO2000A (`104c:8231`) PCIe→PCI bridge | Altera through GPIO | yes ("424b") |

The card is the DSP's own PCI interface. There are no MOTU registers, only the C6000 host port, so the host loads DSP code, talks to it through a control block in DSP memory, and the DSP bus-masters audio into host RAM.

## 2. BARs and DSP memory access

| BAR | Size | Contents |
|---|---|---|
| 0 | 4 MB | Prefetchable window onto DSP memory. Which 4 MB page is visible is selected by DSPP. |
| 1 | 8 MB | DSP peripheral (register) window. |

* For DSP address `a`: if `a >> 23 == 3` (the `0x018xxxxx`–`0x01Fxxxxx` peripheral space), access BAR1 at `a & 0x7fffff`. Otherwise access BAR0 at `a & 0x3fffff`.
* The page is **not** changed implicitly. The caller sets DSPP (`a >> 22`) first. MOTU's `DSP::Read/Write` only masks the address.
* Peripheral reads are byte-swapped in the Mac driver (big-endian heritage). The Linux driver uses raw little-endian `ioread32`.

### Host control registers (BAR1 + `hsr_offset`)

| Offset | Name | Use |
|---|---|---|
| `+0` | HSR | bit1 INTAVAL (DSP→host interrupt pending). Write 4 to mask, 0 to unmask. |
| `+4` | HDCR | 1 = warm reset, 2 = DSPINT (host→DSP interrupt) |
| `+8` | DSPP | Memory-window page (`addr >> 22`) |

### Card-specific parameters (`kParameters` @ 0x4d4a0 / 0x58820)

| | 424a (C6205) | 424b (C6412) |
|---|---|---|
| hsr_offset (in BAR1) | `0x27fff0` | `0x41fff0` |
| RSTSRC (interrupt acknowledge, write `0x10`) | `0x01a40000` | `0x01c00000` |
| program image → dest / clear bytes | `0x00000000` / `0x10000` | `0x00000000` / `0x40000` |
| data image → dest / clear | `0x80000000` / `0x10000` | – |
| boot mailbox | `0x8000fffc` | `0x0003fffc` |
| EMIF CECTL0 value written after boot | `0x10440120` | `0x10914221` |
| Altera bitstream | 59 216 bytes | 37 177 bytes |

### PCIe quirk (`ND424b::InitHook` @ 0x2e210)

If the parent bridge is a TI XIO2000A:
* write `0xff` to the card's config byte `0x0d` (latency timer);
* write `0x20` to the bridge's config byte `0x0c` (cache line size).

## 3. Firmware

All images are zlib-compressed blobs stored as symbols in the kext. `tools/extract_firmware.py` extracts them directly from the `.pkg`.

| File | Symbol source | Size |
|---|---|---|
| `pci424-altera.rbf` | 424a FPGA bitstream | 59 216 |
| `pci424-dsp-prog.bin` | 424a DSP program (→ `0x00000000`) | 37 697 |
| `pci424-dsp-data.bin` | 424a DSP data (→ `0x80000000`) | 13 953 |
| `pci424b-altera.rbf` | 424b FPGA bitstream | 37 177 |
| `pci424b-dsp.bin` | 424b DSP image (→ `0x00000000`) | 28 269 |

DSP images are stored as big-endian 32-bit words. They are written to DSP memory as native dwords after swapping.

## 4. FPGA configuration

The Altera chip is in passive-serial mode, wired to DSP pins and bit-banged by the host through the DSP's register window.

### C6205 (McBSP pins used as GPIO)

* **Init** (`Config_McBSP`): write `[0,0,0,0x20000001,0,0,0,pcr]` to `+0x08..+0x24` of McBSP0 (`0x018c0000`, pcr `0x3800`) and McBSP1 (`0x01900000`, pcr `0x3100`).
* **McBSP0 PCR value** = `nCONFIG?0x08 | 0x3800 | 0x40 | DATA?0x20`.
* **Clock strobe**: McBSP1 PCR = `0x3101`, then `0x3100`.
* **Start**: one strobe, then nCONFIG toggles 0,1,0,1,0,1.
* **Each byte**: bits LSB first. Set DATA, then strobe.
* **End**: up to 100 × (100 strobes, then check CONF_DONE = McBSP0 PCR bit 6). After CONF_DONE is seen, 100 more strobes.

### C6412 (GPIO)

* **Start**: GPEN = `0xe0`, GPDIR = `0xe0`, GPVAL = 0, wait ≥ 1 µs, GPVAL = `0x40`.
* **Each bit** (LSB first): `v = bit ? 0x60 : 0x40`. Write `v`, `v|0x80`, `v`.
* **End**: 1000 × (GPVAL `0xc0`, `0x40`), then GPDIR = `0xc0`. CONF_DONE is not checked.

## 5. Boot sequence (`ND424::StartDSP` @ 0x2d1a0)

1. HDCR = 1 (reset).
2. Configure the FPGA (§4).
3. For each image (program, then data on 424a): set DSPP = `dest >> 22`, zero `clear` bytes at `dest`, then copy the image.
4. Unmask the DSP interrupt (HSR = 0) and write 0 to the mailbox. Then HDCR = 2 (DSPINT) starts the DSP.
5. Poll the mailbox every 5 µs for up to 250 ms. A non-zero value is the DSP address of the **control block (cb)**. On timeout, reset and retry (the Mac driver tries 4 times; the Linux driver tries 5).
6. Read cb `+0x00/+0x04/+0x08/+0x14/+0x18` (`+0x14` is a pointer to the input-routing table).
7. MOTU then writes the fader/input table, sets the sample-rate multiplier and reprograms the clock (see INTERFACES.md).
8. cb `+0x50` = 1 (host ready). EMIF CECTL0 = card value.

The control block is in DSP page `0x200` on the 424a (DSPP stays `0x200` after boot) and in page 0 on the 424b.

## 6. Control block

| Offset | Meaning |
|---|---|
| `+0x14` | → input routing table (24 dwords) |
| `+0x2c` | Current DMA bus address (read by the host to find the period) |
| `+0x50` | Host ready |
| `+0x54` | DSP→host interrupts enable |
| `+0x58` | Idle flag |
| `+0x5c` | Host buffer bus address |
| `+0x60` | Samples per interrupt (`16 << 2n`: 16, 64, 256, 1024) |
| `+0x64` | Interrupts per buffer (3) |
| `+0x68` | Active input slots (0 when idle) |
| `+0x6c` | Active output slots (0 when idle) |
| `+0x80` | Altera configuration word |
| `+0x84` | Clock/address source |
| `+0x98` | Rate constant: `0x05355555` (48 k family), `0x05acbf46` (44.1 k family) |
| `+0xf4` | Stop acknowledge |
| `+0x114` | Commit for `+0x84` |
| `+0x34`–`+0x4c`, `+0x9c`–`+0xf0`, `+0x118`, `+0x11c` | SMPTE reader and generator ([SMPTE.md](SMPTE.md)) |
| `+0x128` | PCI usage statistics |
| `+0x178 + 4p` | S/MUX "Type I" bank mask of AudioWire port p (C6412 cards only; INTERFACES.md §2.3) |
| `+0x188 + 4p` | Second S/MUX mask of port p (the kext writes the same value) |

**Disabling interrupts** (`SetInterruptsEnabled(0)`): write `+0xf4` = 0 and `+0x54` = 0. Then poll `+0xf4` until it is non-zero (about 50 ms).

## 7. DMA ring and sample format

* The buffer is host memory, 32-bit addressable. MOTU aligns it to 4 KB and then adds `0x10`. The Linux driver does the same.
* The ring is **3 periods**. A period is N **blocks**, and each block is **16 frames**. MOTU uses N = 64 (1024 frames).
* A block has `nOut + nIn + 2` **slots** of 12 dwords (48 bytes):

| Slot | Contents |
|---|---|
| 0 | Header |
| 1 … nOut | Playback channels (slot = `outmap[ch] + 1`) |
| nOut+1 … nOut+nIn | Capture channels (slot = `inmap[ch] + 1 + nOut`) |
| last | Status / timestamp. `GetHardwareTimeStamp` reads the last slot of a period's first block. |

* Even when idle, 2 slots per block are DMA'd. MOTU's driver keeps this idle engine running whenever no audio is streaming (`Drv::StartIdling` @ 0x139c0), because the header and status slots carry the MIDI link to the interfaces (INTERFACES.md sections 3 and 4).
* The capture slots of the very first block after the engine starts do not hold audio: on a PCIe-424 with a 24I/O all channels carried the same 16 values. The Linux driver discards the period in which a capture stream starts.
* One slot holds 16 samples of a channel as 24-bit **big-endian** packed audio. Each 3 dwords hold 4 samples and are stored as little-endian dwords:

```
bytes  a2 a1 a0 b2 | b1 b0 c2 c1 | c0 d2 d1 d0
w0 = (a & 0xffffff00) | (b >> 24)
w1 = ((b >> 8) & 0xffff) << 16 | (c >> 16)
w2 = ((c >> 8) & 0xff) << 24 | (d >> 8)          (a..d = left-justified s32)
```

(`PS::clipOutputSamples` @ 0x3b500, `PS::ConvertInputSamples` @ 0x388f0.)

**2× and 4×.** At 88.2/96 kHz the layout is unchanged: one slot per channel, 16 frames per block. At 176.4/192 kHz the card doubles a 2× wire, and every channel has two slots:
* a primary (wire channel `hw`, 0–11) carries the even frames;
* a partner (`hw + 12`) carries the odd frames, each at sample index `(frame >> 1) & 15`.

A block then holds 32 frames. Per AudioWire port, the slots are the port's active primaries, then their partners in the same order. Input routing codes the partner as `hw + 12` like any other channel. The `*2X` converters do this interleave (`ConvertInputSamples2X`, `clipOutputSamples2X`). Only the code has been read: **not hardware-verified**.

### Input routing

* `EnableInput` @ 0x2a650 maintains a 96-byte table. Byte `(slot ^ 3)` = `hw + hw/24`, where `hw` is the wire channel and there are 24 channels per AudioWire port.
* `SetupRouting(in, out, idle)` writes cb `+0x58/+0x68/+0x6c` and then copies the table as 24 dwords to `*cb[+0x14]`.

### Outputs

Playback slots are **not** routed by a table. They are consumed by the DSP mixer program that MOTU's driver *generates* (`MixerState::GenerateMixer`, a C6000 code generator) and downloads (`ND424::DownloadMixer`). Without that program, nothing reaches the outputs; see MIXER.md.

## 8. Interrupts

* The DSP raises INTA (HSR.INTAVAL) once per period.
* The handler checks HSR bit 1 directly (no byte swap), then acknowledges by writing `0x10` to RSTSRC.
* MOTU's `HandleInterrupt` reads cb `+0x2c`, computes the current period from the DMA position, and acts only when the period has changed. `UnpackStatusBuffer` reads `status[8] & 0xf` as the mask of connected interfaces.

## 9. PCI-324

Neither installer can drive the PCI-324 (`137a:0001`):
* The Windows INF/driver matches only devices `0003/0004/0005`.
* In the Mac kext, the 324 classes are stubs and there is no firmware for the card. `Info.plist` matches only `0x3137a`, `0x4137a` and `0x5137a`. `ND324` keeps the old register-based model (DMA slot tables, RAM test, big-endian `ReadCardLong`), but all of its flush methods (`FlushRoutingTable` @0xa930, `FlushHostBufferCfgRegister` @0xa8d0, `FlushOutMapChannelLine` @0xa920, …) are empty. It cannot be used as a reference.
* MOTU's SMPTE Setup app refuses the card ("This application requires a PCI-424 card. It will not work with the PCI-324.").

Supporting the PCI-324 would need an older MOTU driver (Mac OS 9 or Windows XP era) to analyse. Its breakout interfaces (2408, 24i, 308, 1224 mk1, …) are the same AudioWire devices, so most of INTERFACES.md would still apply.
