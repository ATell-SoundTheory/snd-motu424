# MOTU PCI-424: AudioWire interfaces, probing, clocking and the status slot

This document comes from static analysis of MOTU's macOS PCI-424 kext (x86_64 slice, where vmaddr equals file offset).
All addresses in the form `@0x…` are kext addresses.
Class and method names come from the kext's C++ symbols, and `Drv+0x…`, `ND+0x…` and `cb+0x…` are member offsets.
`Drv` is the generic driver object, `ND` (ND424) is the card/DSP object, and `cb` is the DSP control block (see HARDWARE.md).

Every statement cites the code it was derived from.
Statements marked **(uncertain)** are inferences that the code does not prove.

The C tables matching this document are in `linux/snd-motu424/motu424_tables.h`.

---

## 1. Terms

* **Wire / port**: one of the four AudioWire ports on the card. The kext loops over `Drv+0x3c` wires, which is 4 on the 424.
* **Interface / box**: the breakout unit attached to a wire.
* **Hardware channel**: `hw = wire * 24 + c`, where `c` (0..23) is the channel's position on that wire (`Drv::GetBankRelativeID` @0xb8b0).
  * Banks lie one after another inside each wire's 24 slots.
  * Bank `b` of an interface starts at the sum of `GetNumberOfChansInBank(0..b-1)`.
* **Multiplier**: 1 for 44.1/48 kHz, 2 for 88.2/96 kHz, 4 for 176.4/192 kHz (`Drv::SetSampleRate` @0x12990).

---

## 2. Interface types

### 2.1 Identification

Interfaces are **not** identified from the status slot. The status slot only gives a 4-bit "something is connected" mask (section 3).
The type comes from a MIDI SysEx exchange over the wire (section 4).

The reply to the Universal Device Inquiry has these bytes:

| Reply bytes | Field |
|---|---|
| `b[8] \| b[9]<<8` | family |
| `b[10] \| b[11]<<8` | model |
| `b[12]`, `b[13]` | version |

These are read in `Drv::AddDetectedInterface` @0x12dc0.
Each interface class is offered the (family, model) pair through `MatchesSysexID`, in this order:

| Order | Class | Matches | `GetInterfaceID` | Name |
|---|---|---|---|---|
| 1 | `AW_2408` | family 1, model 2 | 2408 (0x968) | "2408" |
| 1 | `AW_2408` | family 1, model 4 | 2403 (0x963) | "2408mk3" |
| 2 | `AW_SimpleInterface` | family 2, any model (@0x1eb40) | 1224 (0x4c8), constant (@0x1eb50) | Read from the box. "interface" until the info arrives (@0x201e0). |
| 3 | `AW_1296` | family 1, model 3 (@0x1dc50) | 1296 (0x510) | "1296" (@0x1e9a0) |
| 3 | `AW_1296` | family 1, model 6 | 192 (0xc0) (@0x1e9d0) | "HD192" |
| 4 | `AW_24IO` | family 1, model 5 | 2410 (0x96a) | "24I/O" |
| 5 | `AW_GenericInterface` | anything else | 9999 (0x270f) | "unknown" (@0x1a7e0) |

Notes on this table:

* The kext uses the name "2408mk3" for family 1 / model 4. There is no "mk2" string: an original 2408 and a 2408mk2 **both report model 2** (hardware-verified). Only their version bytes differ: `11 11` on the 2408 and `11 1A` on the 2408mk2 that were tested.
* **Family 2** ("simple" protocol) boxes describe themselves over SysEx (section 2.6). The **24i is one of them** (hardware-verified). The 1224 and 308 presumably are too **(uncertain: the kext has no model table for them)**.
* The inquiry reply of these three boxes is 15 bytes long, with two version bytes. The 24I/O sends 16 bytes.
* There is no `AW_896` class. The 896 is a FireWire unit and is not an AudioWire box.

### 2.2 Common interface behaviour

These methods come from the vtable of `AW_WireInterface` and its subclasses. The table gives vtable slot offsets.

| Slot | Method | Meaning |
|---|---|---|
| 0x58 | `SupportsSampleRateMultiplier(m)` | The interface can run at multiplier `m` in some way. Base class: `m == 1` only (@0x1a4f0). |
| 0x60 | `SupportsSampleRateMultiplierInHardware(m)` | The interface receives native 2x framing on the wire. Base class: `m == 1` only (@0x1a500). |
| 0xc8 | `GetNumberOfBanks` | |
| 0xd0 | `GetNumberOfChansInBank(b)` | |
| 0xd8 | `GetNthBankPersonality(b, n, str)` | Name of the n-th allowed personality of bank `b`. |
| 0xe8 | `GetPersonalityForBank(b)` | |
| 0xf0 | `DoesNthBankUseAlternateChannelsOnWire(b)` | Base class: false (@0x1a470). |
| 0xf8 | `IsChannelAvailable(c, isOutput)` | `c` is the channel position (0..23) on the wire. |
| 0x100 | `MaybeOverrideChannelName` | |
| 0x130 | `HasVideoSyncInput` | Allows card word sources 4..7 (section 5.1). |

`Drv::GetNthAvailableSampleRate` @0xb4d0 decides which rates are offered:

* Rate index n is `(n odd ? 48000 : 44100) << (n/2)`.
* Multiplier 1 is always offered.
* 2x and 4x are offered only if at least one connected interface returns true from `SupportsSampleRateMultiplier`.

### 2.3 AW_2408 (2408, 2408mk3), @0x1c760–0x1d4b0

**Layout and personalities**

* 3 banks of 8 channels: bank A is c 0–7, bank B is c 8–15, bank C is c 16–23.
* Bank personalities: 0 = ADAT optical, 1 = TDIF, 2 = Analog, 3 = S/PDIF. Only bank C may be S/PDIF.
  * The allowed-personality table `fBankPersonalities` is at @0x4d27c: `00 01 02 ff`, `00 01 02 ff`, `00 01 02 03`.
  * The strings are at @0x28480.
* Exactly one bank is analog. The default is bank A, so the 2408 is 8 analog channels plus 2 × 8 digital channels.

**`IsChannelAvailable` @0x1d0b0**

* Positions with `c % 8 <= 1` are always available.
* If bank C is S/PDIF, only c 16 and 17 exist in bank C.
* In 2x mode (config bit 5), a digital (ADAT/TDIF) bank only has `c % 8 <= 3`.
  * Such banks use "alternate channels on the wire" (`DoesNthBankUseAlternateChannelsOnWire` @0x1ce00: true if 2x is on and the personality is ≤ 1).
  * In other words, a digital bank carries 4 channels at 2x, S/MUX style.
  * The analog bank keeps 8 channels.

**Multipliers** (`SupportsSampleRateMultiplier` @0x1d140)

* 1x on every model.
* 2x only on model 4 (mk3), which also does it in hardware.
* 4x is never supported.

**Clock sources** (`GetNthWordClockSource`)

* 0 "Word Clock In", 1 "Bank A Optical", 2 "Bank B Optical", 3 "Bank C Optical", 4 "SPDIF".
* Choosing a source sets config byte 0 to `0x40 | {0x00, 0x01, 0x09, 0x11, 0x19}` (low bits only).
* `SetWordClockSourceToCard` sets byte 0 to `(b0 & 0xb8) | 0x42`, meaning the box follows the AudioWire/card clock.

**Linux status:** `Interface 2408 Clock Source` exposes these five external
sources as values 0–4 and "follow card" as value 5. `Card Clock Source` can
select internal, ADAT sync, or AudioWire ports 0–3. To use a 2408 as the
external source, select the box source before selecting its AudioWire port;
the reverse order is blocked to prevent a clock feedback loop. The card rate
chosen by the PCM streams is assumed, with no lock/rate detection. This path is experimental
and has not been hardware-tested.

**Config byte 0** (`+0x70`, default 0x42)

* Bits 0–2: clock mode. 0 = word clock in, 1 = digital input, 2 = AudioWire (card).
* Bits 3–4: which digital input (A, B, C or S/PDIF) when the clock mode is 1.
* Bit 5: 2x mode.
* Bit 6: always set.

**Config byte 1** (`+0x71`, default 0x18)

* Bit n (n = 0..2): bank n is TDIF rather than ADAT.
* Bit 3: an analog bank is present.
* Bits 4–5: analog bank index + 1.
* Bit 6: bank C is S/PDIF.

**mk3 extras**

* Video sync input. `SetVideoSyncMode` @0x1cc50 ORs `0x10` (card clock is video), `0x20` (this box is the video master) and `0x40` (LTC is read from this wire) into state byte 14.
* Dedicated SMPTE in/out. `EnableDedicatedSMPTEOutput` @0x1ca60 sets state byte 15 bit 0x02. If bank C is S/PDIF, the SMPTE output overwrites wire slots 16–17 (`GetChannelsUsedByDedicatedSMPTEOutput` @0x1c8c0). See [SMPTE.md](SMPTE.md).

**Device-specific properties** (`SetDeviceSpecificProperties` @0x1d170, `GetDeviceSpecificProperties` @0x1d320; "2408 Options"/"2408mk3 Options" in MOTU PCI Audio Setup, `InterfaceOptions2408`)

* `AnalogMirror` (`+0x72`, 0–2, all models): the bank mirrored on the analog outputs ("Bank to mirror on Analog: Bank A/B/C"). Applied only if no bank is analog (byte 1 bit 3 clear), by writing `AnalogMirror + 1` into byte 1 bits 4–5. The setup app notes that selecting Analog on a bank overrides it.
* mk3 only (model 4):
  * `InputLevels` (`+0x73`): input reference level, one bit per pair Analog 1–2, 3–4, 5–6, 7–8 (bits 0–3). Sent in mk3 state byte 14, ORed with the video-sync bits.
  * `WordOutRange` (`+0x74`, 0–2; 3 is ignored): "Word Out Rate". Selects the SMPTE/flag bits of state byte 15 (note 1).
  * The pane also has an S/MUX "Type I/Type II" option. It is a card setting, not box state (see "S/MUX option" below).

**S/MUX option** (`Drv424::SetSMUXOptionSetting` @0x157b0, `UpdateSMUXOptionSetting` @0x15810, `ND424::UpdateSMUXOptionSetting` @0x2bbe0)

* Only the C6412 cards offer it: `ND424b::GetSMUXOptionCapable` @0x2def0 returns 1, `ND424::GetSMUXOptionCapable` @0x29b20 returns 0. The engine publishes it as `SMUXOptionCapable`/`SMUXOptionEnable` and accepts `SMUXOptionSetting` from the HAL plug-in (`Engine::SetPropertyFromPlugIn` @0x9e66).
* The setting is `Drv+0x38`: 0 (default, `InitWithNumberOfWires` @0x1159a) = "Type II", 1 = "Type I". The setup app's radio buttons are 0x5208 "Type I" (checked when the setting is non-zero) and 0x5209 "Type II".
* The update runs after a change and at the end of every configuration session (`Drv::CloseConfigSession` @0x11096). For each AudioWire port p (0–3):
  * If the setting is 1 and the port holds a 2408mk3 (`GetInterfaceID` = 2403), build `mask` with byte n = 0xff for every bank n whose personality is ADAT (0). TDIF, analog and S/PDIF banks contribute 0.
  * Otherwise `mask` = 0.
  * Write `cb+0x178+4p = mask` and `cb+0x188+4p = mask` with `WritePolitely`. The kext writes the same value to both words. They are probably the input and output side **(uncertain)**.
* The masks are written at every rate; the DSP presumably applies them only to 2x ADAT banks **(uncertain: the DSP side has not been analysed)**. Other boxes, including the 2408/2408mk2, always get 0.
* Both connected 2408s here report model 2, so only `AnalogMirror` applies to them.

**Linux status (S/MUX):** `Card S/MUX Type` (C6412 cards only) holds the setting. Each port's mask is recomputed from the confirmed box state after layout changes and boots, and rewritten when it changes. A box that is being reconfigured keeps its old mask until it confirms. The DSP effect is **not hardware-tested** (no 2408mk3 available).

**Linux status:** ALSA controls select the analog bank (or none) and the ADAT/TDIF
personality of each bank, plus S/PDIF on bank C. The driver updates the PCM
layout from the confirmed state. Bank changes are blocked while a PCM stream
is open. `AnalogMirror`, and the mk3 `InputLevels` and `WordOutRange`, have
their own controls; they don't change the layout and may be changed while
streaming. The mk3 byte 15 is recomputed from the clock mode on every send.
A model-2 2408 echoed the no-analog-bank and mirror states (byte 1 = 0x10,
0x20). The audio paths of the bank changes and all mk3 settings are
**not hardware-tested**.

### 2.4 AW_24IO (24I/O), @0x1d540–0x1dbc0

* 1 bank of 24 analog channels. `IsChannelAvailable` is `c <= 23`, so every channel is available, including at 2x **(uncertain: whether 2x halves the usable count is not checked here)**.
* Multipliers 1 and 2.
* Clock source: "Word Clock In" only. Otherwise the box follows the card.

**Device-specific properties** (`SetDeviceSpecificProperties` @0x1d8a0, `GetDeviceSpecificProperties` @0x1d9b0; "24IO Options" in MOTU PCI Audio Setup, `InterfaceOptions24IO`)

* `InputLevels` (`+0x71`): "Input Reference Level", state byte 11 bits 0–2. Bit n covers Analog 8n+1–8n+8. Clear = +4 dBu (the first radio button), set = −10 dBV. The default is 0 (all +4 dBu).
* `WordOutRange` (`+0x72`, 0–2; 3 is ignored): "Word Out Rate", state byte 15. 0 "Match system clock" follows config bit 5 (2x); 1 "44.1/48 (divide 88.2 or 96 by 2)" sends 0; 2 "88.2/96 (multiply 44.1 or 48 by 2)" sends 1.
* A change is sent through `SendSetup` with confirmation. The reply check compares bytes 10, 11 and 15, so the box echoes both settings.
* These are the only box settings for the 24I/O. Neither the kext nor the setup app has phantom power, preamp gain or pad controls for any PCI-424 interface.

**Linux status:** both settings have ALSA controls and are kept in bytes 11
and 15 of every state message, including the multiplier change and the release.
Hardware-tested: the box echoes both bytes. A −40 dBFS 1 kHz loopback from
analog out 19 to analog in 19 read −40.1 dBFS at +4 dBu and −26.5 dBFS with
Analog 17–24 at −10 dBV.

### 2.5 AW_1296 (1296 and HD192), @0x1dc40–0x1eb10

**Layout**

* 1 bank reported as 24 positions (@0x1dd00).
* `IsChannelAvailable` is `c <= 11` (@0x1dd30). Each box therefore has **12 inputs and 12 outputs**, using wire positions 0–11.

**Multipliers**

* `SupportsSampleRateMultiplier` @0x1e970: 1 and 2 on both models; 4 only on the HD192 (model 6).
* `SupportsSampleRateMultiplierInHardware` @0x1dc80: 1 on both; 2 only on the HD192.
  * At 2x, a 1296 is therefore driven in "card-side doubling" mode (section 5.4). The 12 channels then occupy all 24 wire slots **(uncertain, but consistent with only 12 positions being exposed)**.
* `SetSampleRateMultiplier` @0x1e9f0: on the HD192, a multiplier above 1 is halved before it is sent to the box. This sets an internal flag `+0x7f`, and `+0x75` = (halved value == 2).
  * So at 4x the HD192 runs 2x framing on the wire, and the card supplies the extra doubling.

**Clock sources** (`GetNthWordClockSource` @0x1e550)

* 0 "Internal", 1 "Word Clock In", 2 "AES/EBU".
* AES/EBU is hidden on the HD192 when its `+0x75` (4x) flag is set.

**Channel names** (`MaybeOverrideChannelName` @0x1ea80)

* One stereo pair can be switched to AES/EBU.
* The selected input pair (`+0x78`, `AESInputSteal`, 1-based; 0 = none) is renamed "AES/EBU". `IsInputAnalog` @0x1ea30 is false for that pair.
* The output pair with 0-based index `+0x79` (`AESOutputSRCMode`) is renamed "Analog/AES". The default value 0 therefore marks outputs 1–2.
* Neither name is used on the HD192 at 4x.

**State fields** (`AW_1296::state` is at `+0x18`, so `state+0x5c` = `AW_1296+0x74`)

| `AW_1296` | Default | Meaning | Message bits |
|---|---|---|---|
| `+0x74` | 1 | Active; `Breakdown` clears it | c0 bit 6 |
| `+0x75` | 0 | 2x framing on the wire | c0 bit 4 (and bit 5 on the 1296) |
| `+0x76` | 1 | Clock code (note 3) | c0 bits 0–2 |
| `+0x77` | −1 | UI clock: −1 card, 0 internal, 1 word clock, 2 AES/EBU | via the clock code |
| `+0x78` | 0 | `AESInputSteal` (0–6) | c1 bits 4–6; 0 on the HD192 at 4x |
| `+0x79` | 0 | `AESOutputSRCMode` (0–5 or 8–13) | c1 bits 0–3 |
| `+0x7a` | 0 | `AESOutputClock` (0–10) | via the computed `+0x7b` |
| `+0x7b` | 0 | Computed AES output clock | c2 bits 0–3 |
| `+0x7c` | 0 | `AESInputSRC` | c0 bit 3 = not set |
| `+0x7d` | 4 | `PeakHoldTime` (0–7) | c3 bits 3–5 |
| `+0x7e` | 1 | `ClipHoldTime` (0–7) | c3 bits 0–2 |
| `+0x7f` | 0 | HD192 4x | c0 bit 5 on the HD192, c2 bit 5 |

* The names in backticks are the keys of `Set/GetDeviceSpecificProperties` @0x1e5b0/0x1e7f0. Out-of-range values are ignored.
* `MOTUAW.sys` (Windows x64, constructor at VA 0x11480) initializes the same fields to the same values, and has the same message templates at VA 0x35270.

**AES output clock** (`GetClockFromUIClock` @0x1ddd0, jump table @0x1de38)

* `AESOutputClock` 0 → 0; 1 → 0 if the box clock is AES/EBU, otherwise 1; 2 → 2.
* 3–6 are 44.1/48/88.2/96 kHz: the value is kept, except that it becomes 0 if the system rate is equal and the card clock is internal.
* 7–10 are the same rates: the value becomes 0 whenever the system rate is equal.
* Larger values are passed through. The meaning of 1 and 2 is **uncertain** (probably "follow the input" and "follow the system clock").

**Linux status:** the default state is sent, with the clock code set to "follow the card". ALSA controls for `AESInputSteal`, `AESOutputSRCMode`, `AESOutputClock`, `AESInputSRC`, `PeakHoldTime` and `ClipHoldTime` update the state bytes and resend the state/request pair. The output-clock value is translated using the current card rate and MOTU's single-speed logic. `Interface 1296 Clock Source` selects the box clock with the UI values of `AW_1296::state::GetClockFromUIClock` @0x1ddd0 (0 internal, 1 word, 2 AES, 3 = UI −1, the card). The internal clock code follows the card's base-rate family. On the HD192 at 4x, AES falls back to internal, because `GetNthWordClockSource` does not offer AES there. Like the 2408 clock, it cannot be changed while PCM is open or while the card follows that port. The setup and controls are **not hardware-tested**.

### 2.6 AW_SimpleInterface (family 2), @0x1eb40–0x20410

These boxes describe themselves. Until the description is complete (`+0xa8` set), the kext treats the box as 1 bank of 24 channels with no channel available (@0x1fe30), and it sends no state.

**Description requests** (`AW_SimpleInterface::info`, started by `GetInfo` @0x1f290)

* A request is `F0 00 00 33 20 7F 02 20 <step> F7` (`kSIMPLE_REQUEST` @0x3e3d7, byte 8 patched by `DoStep` @0x1f210).
* A reply carries its data from byte 7 up to the F7. Byte 6 is a checksum: the sum of the data bytes, 7 bits (`info::CheckSum` @0x1ec10). A 24i answers with `F0 00 00 33 21 <step> <checksum> <data…> F7`.
* A bad checksum or a timeout (0x60e ticks) repeats the request. The kext has no retry limit here; the Linux driver gives up after 10 attempts.
* The steps run in this order (`info::ReplyReceived` @0x1f5e0):

| Step | Reply data | Stored at |
|---|---|---|
| 2 | bank count, clock-source count, then the box name as text | `+0x40f`, `+0x410`, `+0x54` |
| 3 | one type code per bank | `+0x3ae[]` |
| 4 | channels per bank | `+0x3be[]` |
| 5 | one type code per clock source | `+0x3c6[]` |
| 6 | 8 bytes of channel bitmaps | `+0x3a6[]` |
| 9 | digital connectors per bank. Asked only if the version word is above 0x300. | `+0x3b6[]` |

* The version word is inquiry reply `b[10] << 8 | b[11]` (`Setup` @0x1fd10, `SupportsNumDigiConsMsg` @0x1ebd0). So for family 2 the "model" bytes are a version. Without step 9, `SetupNumBankConsForOldInterface` @0x1ee30 derives the connector counts from the bitmaps and bank types.
* **Custom names** (`info::DoGetTypeInfo` @0x1f2b0). After step 3 and after step 5, each entry with type code 0x7F is asked for its name with `F0 00 00 33 20 7F 02 20 <list> <index> F7` (`kSIMPLE_STRREQUEST` @0x3e3e1; list 7 = banks, 8 = clock sources). The name is the text from reply byte 8 on.

**Bank type codes** (string table @0x28360): 0 Analog, 1 ADAT, 2 TDIF, 3 SPDIF, 4 AES/EBU, 5 Sony, 6 TOS-Link, 0x7F custom name.

**Channel bitmaps** (`info::IsChannelAvailable` @0x1ecb0)

* Each byte holds 7 channel positions (MIDI-safe 7-bit data): position c is bit `c % 7` of byte `c / 7`.
* Bytes 0–3 are the **inputs** and bytes 4–7 are the **outputs**.
* This allows up to 28 positions per direction, of which 24 are used.

**Clock sources** (`info::GatherClockTypeInfo` @0x1ed20, string table @0x283d0)

| Type code | Meaning | kext name |
|---|---|---|
| 0 | follow the card | "PCI-324" |
| 1, 2, 3 | internal at 44.1k, 48k, 96k | "Internal" |
| 4 | word clock input | "Word clock" |
| 5–12 | digital input of bank A–H | "Bank A" … "Bank H" |
| 0x7F | custom name | |

* The kext shows one "Internal" entry and picks type 1, 2 or 3 from the sample rate (`info::GetClockFromUIClock` @0x1f020).
* **The clock setting is the index into the box's own list**, not the type code.

**State message** (`SendCurStateToBox` @0x203c0)

* `F0 00 00 33 20 7F 02 01 01 00 <clock> F7` (`kAW_SI_Message` @0x3e3ec), where `<clock>` = `0x40 | index`. The default index is that of the type-0 source, so the box follows the card.
* It is sent with confirmation (section 4.1 note 5). The reply is good if it has at least 12 bytes and byte 10 matches (`Sender::ReplyGood` @0x1ebf0).
* On close, `Breakdown` @0x1fcc0 sends the index without the 0x40 bit.

**Hardware-verified on a 24i** behind a PCIe-424:

* Inquiry reply `F0 7E 00 06 02 00 00 33 02 00 03 00 10 10 F7`: family 2, version word 0x0300, so step 9 is not asked.
* Step 2: 1 bank, 4 clock sources, name "24i". Step 3: Analog. Step 4: 24 channels.
* Step 5: clock sources are word clock, card, internal 44.1k, internal 48k. The card is index 1.
* Step 6: `7F 7F 7F 07 03 00 00 00`, which is 24 inputs and 2 outputs.
* State `… 41 F7` is echoed as `F0 00 00 33 20 00 02 00 01 00 41 02 00 F7`. The 48k LED is then lit with the card at 48 kHz, and all 24 inputs deliver audio.

### 2.7 AW_GenericInterface (unknown box), @0x1a6e0

* 1 bank of 24 channels, and every channel is available (@0x1a720). The name is "unknown".
* `Drv::ProbeForInterfaces` retries the probe up to 5 times while any wire still holds a generic interface (section 4).

### 2.8 Channel tables (Drv+0x3ec0 inputs, Drv+0x434c outputs)

`Drv::MaybeRebuildActiveInTable` @0xbde0 and `MaybeRebuildActiveOutTable` @0xcc40 rebuild the tables whenever a dirty flag (`Drv+0x3d34`) is clear.

**Per-channel records**

* There is one record per hardware channel, `Drv+0x4c + 0x2c*hw` for inputs (the output records sit at +0x10cc).
* The fields are:
  * `+0x4e`: the channel exists. It is filled from `IsChannelAvailable` **(uncertain: the exact setter was not traced)**.
  * `+0x4c`: enabled.
  * `+0x4f`: active, which is computed.
  * `+0x50`: kind. 2 = an alternate/doubling partner channel.

**Active list**

* Every `hw` from 0 to `wires*24-1` is scanned in ascending order.
* A channel that exists and is enabled is appended to `Drv+0x3ec0[]` (inputs). The count is kept at `Drv+0x3eb8`.
* `Drv+0x3d38[]` gets the same order, followed by the inactive channels.

**Doubling partners**

* These are kind-2 records whose primary channel is active.
* They are collected separately at `Drv+0x4040[]`, with the count at `Drv+0x3ebc`.
* When `Drv+0x44` is set (the card itself doubles, i.e. 4x), the active count is added to `Drv+0x3ebc`. Each active channel then occupies two DMA slots.

**DMA slot counts** (`Drv::StartAudioRunning` @0x13a80)

* Inputs: `Drv+0x3eb8 + Drv+0x3ebc`.
* Outputs: `Drv+0x4344 + Drv+0x4348`.
* So the DMA stream carries only active channels, in ascending hardware-channel order (see HARDWARE.md for the frame layout).
* **Hardware-verified** with four boxes (24I/O, 2408, 24i, 2408mk2) on a PCIe-424:
  * inputs: capture slot s is routed to hardware channel `in_hw[s]` with table byte `(s ^ 3) = hw + hw/24`, and each box's inputs arrive in its slot range;
  * outputs: with 24, 24, 2 and 24 output channels the fourth box's first output is playback slot 50, directly behind the 24i's two slots;
  * after one box is switched off, the slots of the boxes behind it move up, for inputs and outputs alike.

**Channel names** (`Drv::DoRefreshChannelInfo` @0x10520 and `Drv::GetChannelName` @0x10380)

* Names have the form `"<interface name>:<bank/channel name>"`, optionally with `-<letter>`.
* Bank names come from the personality strings. Banks with the same personality on one interface are disambiguated as `"%s-%d"` (for example "ADAT-1", "ADAT-2").
* The exact per-channel numbering text was not traced **(uncertain)**. `motu424_tables.h` assumes `"<bank> <n>"`, with n counted from 1 within the bank.

---

## 3. The status slot

Each 16-frame block ends with a status slot of 12 dwords; there are 12 longs per slot because `Drv+0x3d30` = 12 (@0x11ac4).
`Drv424::UnpackStatusBuffer` @0x19ee0 decodes it. The dword indices are:

| dword | Content |
|---|---|
| `s[0..3]` | MIDI **input** from wire 0..3. Bit 24 = byte valid, bits 16–23 = the byte. In MIDI-doubled mode a second byte is in bits 0–7, with bit 8 as its valid flag. |
| `s[4]` | MIDI input for "destination 4" (a fifth MIDI port, card-level **(uncertain)**). Two bytes: bit 24 valid + bits 16–23, then bit 8 valid + bits 0–7. |
| `s[5]`:`s[6]` | 64-bit hardware sample counter (high:low). Multiplied by the multiplier (`Drv+0x40`) to get the timestamp (`Drv424::GetHardwareTimeStamp` @0x16190). |
| `s[7]` | Used **instead** when the card word source is 2 (ADAT sync): timestamp = `s[7] - 0x57e400 - 0x2d` (@0x16190). |
| `s[8]` | Bits 0–3: **present mask** (one bit per wire; stored at `Drv+0x20dbc`). Bits 6–9: in doubled mode, which half carries wire 0..3's MIDI byte. |
| `s[9..11]` | Not used by the kext **(unknown)**. |

**Doubled MIDI** (`ND424::IsMIDIDoubled` @0x2a920)

* This applies when the multiplier is greater than 1 and the wire's bit is set in the "not native 2x" mask `ND+0x1f0` (section 5.4).
* The driver toggles `Drv+0x20daa` on every block. On "odd" blocks it ignores that wire.
* On "even" blocks it takes the low byte if `s[8]` bit `6+wire` is set, and the high byte otherwise.

**Present-mask debounce** (`ND424::SetDevicePresentMask` @0x2ab80)

* Each wire needs 100 consecutive periods with its bit set (counters at `ND+0x1fc..0x208`) before it counts as present.
* A change makes `Drv424::HandleInterrupt` @0x19c30 call `Drv::SetOfflineMask`, then `Drv::ProbeUsingPresentMask` @0x110b0.
* The same function counts down the "Altera settle" timer (section 5.3).

**Other per-period work**

* `Drv424::HandleInterrupt` also performs SMPTE frame-rate detection.
* `GetDetectedFrameRate` returns 0x18, 0x19, 0x1d or 0x1e, which map to formats 0–3 = 24, 25, 29.97 and 30 fps (table @0x3e2e0).

### 3.1 MIDI output (host → box)

**Timing and placement**

* `Drv::DoMIDI` @0x13660 runs once per 16-frame block (`Drv+0x466c` times per period).
* For each block, it reads the status slot of the block that just completed (`DoOneMIDIPacket` @0x135a0).
* It writes an outgoing packet into **dword 0..10 of the first slot (the "header" slot) of the matching block in the next period** (`MIDI::PackNextMessage` @0x1c0f0).

**Packet format**

* The packet is cleared to zero first, and dword 0 is unused.
* Destination `d` (0..4) uses dword `1 + 2d`, with value `(byte << 8) | kDestValidBits[d]`.
* `kDestValidBits` @0x3e360 = `{0x20000, 0x40000, 0x80000, 0x100000, 0x10000}`.
* In MIDI-doubled mode (`ShouldSendMIDITwiceToDest`: d ≤ 3 and doubled), the same word is also put in dword `2 + 2d`.

**Rate and mapping**

* Each destination has its own pacing (`SubQueue::GetNextMessage` @0x1b420). After a byte, the next one is due `1 + gap` ticks later, where a tick is one block at 1x.
  * The defaults set by `SubQueue::SubQueue` @0x1ad20 are an inter-byte gap of 3 and an inter-message gap of 30. So a wire gets **one byte every 4 blocks** (64 frames) and **31 blocks** between the end of one message and the start of the next.
  * `MIDI::init` calls `SetDestSpeed(4, 2, 4)` (@0x11a99), which changes this only for destination 4: one byte every 3 blocks and 5 blocks between messages.
  * **Hardware-verified:** a 24I/O does not answer when it is sent one byte per block. With the 4-block pacing it answers immediately.
* Wire n maps to MIDI destination n (`kWireToMIDIDestMapping` @0x3e2c0 is the identity).
* The tick counter advances on every block at 1x. In 2x wire mode (`Drv+0x45`) it advances on every second block (`Drv::DoOneMIDIPacket` @0x135a0).

**Inbound assembly**

* `Drv424::UnpackStatusBuffer` @0x19ee0 takes a wire's MIDI byte only while that wire's present bit is set.
* `Inbuffer::Take` only reassembles SysEx (`F0..F7`, up to 0x400 bytes) for the driver's own use.
* Any other MIDI bytes are passed through as port MIDI.

**Linux status:** The driver exposes four duplex ALSA rawMIDI substreams,
one per AudioWire port (subdevices 0–3). MOTU interface-control SysEx and
Universal Device Inquiry replies are consumed by the driver; other input
bytes, including unrelated SysEx, are forwarded to that port. Host output is
held until the interface is configured and the control-message queue is clear,
then sent at one byte per four MIDI ticks. This path is not hardware-tested.
The card-level destination 4 is not exposed; its purpose remains uncertain.

---

## 4. Probing

**Overall flow** (`Drv::ProbeForInterfaces` @0xf760)

1. Set the card word source to 0 (internal) and sleep 150 ms.
2. Release all interface objects and wait for MIDI to drain.
3. For up to 9 rounds, send the inquiry to every wire whose present bit is set and has no interface. The waits between rounds are 50 ms and 150 ms.
4. `RemoveUnsupportedInterfaces`.
5. Restore the word source.
6. Retry the whole sequence up to 5 times if any wire ended up generic.

**Inquiry**

* The message is the MIDI Universal Device Inquiry `F0 7E 7F 06 01 F7` (`kUnivDevInquiry` @0x3e374), sent by `AW_InterfaceQuery::StartQuery` @0x1c700.
* Replies longer than 11 bytes are saved (16 bytes) to `Drv+0x3ab8 + 16*wire` (`Drv::InterfaceReplyReceived` @0x130c0).
* MOTU's SysEx manufacturer ID is `00 00 33`.

**Background re-probe** (`ProbeUsingPresentMask` @0x110b0)

* A wire that is present but has no interface is queried again with a back-off of `60 << n` ticks, where n ≤ 4.

**`AddDetectedInterface` @0x12dc0**

1. Pick the class from the (family, model) pair (section 2.1).
2. Call `Setup(drv, wire, family, model, reply)` and `SetupInterfaceState`, then `SendCurStateToBox`.
3. Set the per-wire MIDI busy counter `Drv+0x3b10 + 0x28*wire` to 6.

**`InterfaceSupportedByCard` @0xdbb0**

* The interface is accepted if the card can do 2x in hardware, which the 424 always can (`Drv424::SupportsSampleRateMultiplierInHardware` @0x15750: m ∈ {1, 2}).
* It is also accepted if the interface does not need 2x.

### 4.1 Host → box control messages (SysEx over the wire's MIDI channel)

All of these messages are sent through the MIDI output path of section 3.1, to the wire's destination.

| Message | Bytes / layout | Source |
|---|---|---|
| Device inquiry | `F0 7E 7F 06 01 F7` | @0x3e374 |
| 2408 state | `F0 00 00 33 20 7F 02 01 01 00 b0 b1 F7` (13 bytes, see note 1) | `kAW_2408_Message` @0x3e380 |
| 2408 state request | `F0 00 00 33 20 00 02 20 01 F7` | @0x3e391 |
| 24I/O state | 17 bytes (see note 2) | `kAW_24IO_Message` @0x3e3a0 |
| 1296/HD192 state | `F0 00 00 33 22 01 00 c0 c1 c2 c3 00 00 00 00 F7` (16 bytes, see note 3) | `kAW_1296_Config_Message` @0x3e3c0, `PackState` @0x1e050 |
| 1296 request | `F0 00 00 33 23 01 F7` | @0x3e3d0 |
| Simple: description request | `F0 00 00 33 20 7F 02 20 <step> F7` (section 2.6) | `kSIMPLE_REQUEST` @0x3e3d7 |
| Simple: name request | `F0 00 00 33 20 7F 02 20 <list> <index> F7` (section 2.6) | `kSIMPLE_STRREQUEST` @0x3e3e1 |
| Simple: state | `F0 00 00 33 20 7F 02 01 01 00 <clock> F7` (section 2.6) | `kAW_SI_Message` @0x3e3ec |
| Simple: state request | `F0 00 00 33 20 00 02 20 01 F7` (same bytes as the 2408 request) | `kAW_SI_Request` @0x3e3f8 |
| Power-up | `F0 00 00 33 20 7F 02 03 52 F7` | `kInterfacePowerUpMsg` @0x4d26a |
| DTP query | `F0 7F 7F 06 42 01 40 F7` | @0x4d274 |

Notes:

1. **2408 state** (`AW_2408::SetMessageBytes` @0x1c940). b0 and b1 are the 2408 config bytes from section 2.3. `Setup` @0x1d410 starts with b0 = 0x42 (follow the card) and b1 = 0x18 (bank A analog, banks B and C ADAT).
   * The mk3 version is 17 bytes:
     * byte 12 = 0x7F, byte 13 = 0x7F;
     * byte 14 = `InputLevels` (bits 0–3) | video-sync bits (0x10/0x20/0x40);
     * byte 15 = flags: 0x20 if the clock mode is 2; `WordOutRange` 1 → none, 2 → 0x01, otherwise 0x10 (default); 0x02 if the dedicated SMPTE out is on;
     * byte 16 = F7.
   * The reply is good if it is at least as long as the message and bytes 10 and 11 match; the mk3 also compares bytes 14 and 15 (`Sender::ReplyGood` @0x1c860).
   * On close (`Breakdown`), the driver sends b0 = 4, b1 = 0.
   * **Hardware-verified** on a 2408 and a 2408mk2 behind a PCIe-424, with the 13-byte message `… 42 18 F7`:
     * the echoed state is 15 bytes, `F0 00 00 33 20 00 02 00 01 00 42 18 s0 s1 F7`. The two extra bytes were `03 08` on the 2408 and `00 07` on the mk2 **(meaning unknown)**;
     * both boxes then light "48k" and "bus" with the card at 48 kHz;
     * the 8 inputs of bank A deliver analog noise, and banks B and C deliver digital silence with nothing connected to the optical inputs.
2. **24I/O state** (`AW_24IO::SetMessageBytes` @0x1d6e0). The bytes are:
   * byte 10 = config byte (default 0x42; bit 5 = 2x; bits 0–2: 2 = card clock, 0 = word clock);
   * byte 11 = `InputLevels` & 7 (section 2.4);
   * bytes 12–14 = 7F 7F 7F;
   * byte 15 = word-clock-out range: 0 or 1 if the setting `+0x72` is 1 or 2, otherwise bit 5 of the config byte;
   * byte 16 = F7.
   * The 24I/O state request (@0x3e3b1) has the same bytes as the 2408 request.
   * `Breakdown` sends 3 in byte 10.
   * The box has **no sample-rate field**. With config 0x42 it follows the card's clock and shows the rate it detects.
   * **Hardware-verified** on a 24I/O (firmware bytes `11 10 11`) behind a PCIe-424:
     * inquiry reply `F0 7E 00 06 02 00 00 33 01 00 05 00 11 10 11 F7`;
     * echoed state `F0 00 00 33 20 00 02 00 01 00 42 00 00 00 7F 00 F7`.
     * Before the state message the 44.1k LED flashes (no valid clock). Afterwards the 48k LED is steady with the card at 48 kHz.
3. **1296/HD192 state.** The config bytes are:
   * c0 = clock code (0–4) | !AESInputSRC <<3 | 2x <<4 | (HD192 ? 4x-flag : 2x) <<5 | active <<6;
   * c1 = `AESOutputSRCMode & 0xf` | `(AESInputSteal & 7) << 4`;
   * c2 = computed AES output clock `& 0xf` | send-arg <<4 | 4x-flag <<5;
   * c3 = `(PeakHoldTime & 7) << 3` | `ClipHoldTime & 7`.
   * Field offsets and defaults: section 2.5.
   * Clock code (`GetClockFromUIClock` @0x1ddd0): 1 = follow the card; 3 = internal at 44.1/88.2/176.4k; 4 = internal at 48k-family rates; 2 = word clock; 0 = AES/EBU. On the HD192 at 4x, AES/EBU is replaced by internal.
   * Send-arg (c2 bit 4, meaning **unknown**): `SendCurStateToBox` 1, `SetWordClockSource(ToCard)` and `Breakdown` 0, `SetSampleRate` its flag, `SetSampleRateMultiplier` 1 (blind), `SetDeviceSpecificProperties` 1 if an AES field changed.
   * `AddDetectedInterface` ends with `SendCurStateToBox`. With the defaults and the card clock, the first state is therefore `… 49 00 10 21 00 00 00 00 F7`; `Breakdown` sends `… 09 00 00 21 …`.
   * The reply is good if it is longer than 10 bytes, bytes 7, 8 and 10 match, and byte 9 matches except for bit 4 (`ReplyGood` @0x1df80). The reply format itself has not been observed.
   * `ClearClippingIndicators` @0x1e180 writes F7 into byte 7 and blind-sends all 16 bytes (`F0 00 00 33 22 01 00 F7 c1 c2 c3 00 00 00 00 F7`). The box presumably sees an 8-byte message; how `MIDI::EnqueueMsg` handles the bytes after the first F7 has not been checked.
4. **Other messages.**
   * The kext also knows `F0 00 00 0E` (Alesis) for ADAT-sync SysEx (@0x3e2d0).
   * `AW_24IO` and `AW_2408` resend state with `BlindSend` (no reply expected) when the multiplier changes.
5. **Sending state with confirmation** (`MsgSender::SendAgain` @0x1a840).
   * Each attempt queues the state message and then the state request. The reply timeout is 0x60e ticks (`CalcStdWireInterfaceTimeout` @0x1ad10), counted from the moment the request has been sent.
   * The box answers the request with its state. For the 24I/O the reply is good if it is at least 17 bytes long and bytes 10, 11 and 15 equal those sent (`AW_24IO::Sender::ReplyGood` @0x1d620).
   * A bad reply or a timeout sends everything again, up to 10 attempts in total.
6. **Power-up.** When a box sends the power-up message (byte 5 is not compared), `Drv::HandleUntrappedMIDIMsg` @0x13200 starts a new device inquiry on that wire. A 2408, a 2408mk2 and a 24i all send `F0 00 00 33 20 00 02 03 52 F7` when they are switched on (hardware-verified).

---

## 5. Clocking

### 5.1 Card word source (`ND+0x38`)

**Supported values** (`Drv424::IsWordSourceSupported` @0x15b00)

* Values 0, 2, 3 and 8–11 are always allowed.
* Values 4–7 are allowed only if the interface on wire `ws-4` has a video-sync input (`HasVideoSyncInput`, slot 0x130).
* Value 1 is not supported on the 424.

**Linux status:** `Card Clock Source` accepts 0, 2, 3 and 8–11, plus 4–7 when wire `ws-4` holds a 2408mk3 (the only box with `HasVideoSyncInput`). Every clock change rewrites the address source (`cb+0x84`/`cb+0xa8`, [SMPTE.md](SMPTE.md) §3.1) and the 2408mk3 video/LTC bits.

| ws | Meaning (strings @0x28300 and code paths) | `kModes[ws]` @0x3e440 |
|---|---|---|
| 0 | Internal | 0x200 |
| 1 | Control track (not on the 424) | 0x200 |
| 2 | ADAT sync (9-pin) | 0x800 |
| 3 | SMPTE (LTC; [SMPTE.md](SMPTE.md)) | 0x201 |
| 4–7 | Video sync via the interface on wire 0–3 | 0x202 |
| 8–11 | AudioWire clock from the interface on wire 0–3 | 0x400, 0x500, 0x600, 0x700 |
| 12–15 | (unused) | 0 |

### 5.2 Writes per event

All of these writes are 32-bit stores to `cb + off` through `ND424::DSP::Write` @0x29d80. The kext uses no extra handshake or read-back on these words.

**A. Base rate family** (`Drv::SetBaseSampleRate` @0x102c0 → `ND::SetSampleRate48k` @0xa1d0 → `ND424::SampleRateChanged` @0x2d880)

* It runs only when the 44.1k/48k family actually changes.
* `cb+0x98` = `0x05355555` (48k family) or `0x05ACBF46` (44.1k family).
* The value depends only on the family and not on the multiplier.
* Afterwards the kext recomputes free-wheel and SMPTE parameters. `UpdateSMPTEParameters` writes `cb+0xbc/0xe4/0xf0` "politely", then `cb+0x114 = 1`.

**B. Multiplier and wire mask** (`Drv424::SetSampleRateMultiplier` @0x19a50 → `ND424::SetSampleRateMultiplier` @0x2bf70)

* `ND+0x1ec` = card multiplier and `ND+0x1f0` = wire mask. The present-mask debounce counters are reset.
* `alt` = `MakeAlteraConfiguration()` @0x2a8f0:
  * `0x80000000` if the multiplier is ≤ 1;
  * otherwise `0x80001000 | (mask << 24) | ((mask ^ 0xf) << 20)`.
* If `alt` changed, the kext stores it, starts a **settle timer** of 40000 frames (`ND+0x20c` = 0x9c40), and writes `cb+0x80` (section C).
* Then it calls `SendOneXClockBits` (section D).

**C. `cb+0x80`** (`ND424::SendAlteraConfiguration` @0x2aa70)

* `idx` = `ws`, except that `idx` = 0 while the wire-clock hold timer `ND+0x210` is running or if ws > 0x2f.
* If 4 ≤ idx ≤ 7, the kext first writes `cb+0xac = 25*(idx-4) + 0x18`.
* `cb+0x80 = alt | kModes[idx] | (settled ? 0xF0000 : 0)`, where `settled` means `ND+0x20c == 0`.
* The settle timer is decremented by `samples-per-interrupt` every period in `SetDevicePresentMask` (@0x2ac5f).
  * When it reaches ≤ 0, `cb+0x80` is rewritten with the `0xF0000` bits set. With the fixed 1024-frame period, that takes about 40 interrupts.
  * The timer restarts (and `cb+0x80` is rewritten without `0xF0000`) whenever a debounced present bit changes while the multiplier is above 1 (@0x2ad71).
  * **(uncertain)** what `0xF0000` enables. It may be the per-wire output enables.

**D. `cb+0x88/0x8c/0x90`** (`ND424::SendOneXClockBits` @0x2a950)

| Condition | +0x88 | +0x8c | +0x90 |
|---|---|---|---|
| default (hold running, or ws ≤ 7 and ws ≠ 2) | 1 | 0x80000 | 0 |
| ws = 2 (ADAT) | 1 | 0x40000 | 0 |
| ws = 8+w, wire w's bit set in mask (doubled) | 1 | `1 << (w + 14)` | 0 |
| ws = 8+w, wire native | `25*w + 0x1c` | 0x80000 | 0x20000 |

**E. Word source change** (`Drv::SetCardWordSource` @0xfb80 → `ND::SetWordSource` @0xa1b0 → `ND424::WordSourceChanged` @0x2aff0)

1. `ND+0x210` = `ws > 7 ? multiplier * 48000 : 0`. This is a hold of about one second: the card stays on its internal clock (kModes[0]) while the box locks.
2. Write `cb+0x80` (C) and `cb+0x88..90` (D).
3. `ND424::SendAddressSource` @0x2af50:
   * `cb+0x84` = 1 if ws = 2. Otherwise, if `ND+0x278` ≠ 0 and 3 ≤ ws ≤ 7, it is `ND+0x27c ? 3 : 2`. Otherwise it is 0.
   * Then `cb+0xa8 = ND+0x278`.
   * Then **`cb+0x114 = 1`** (commit).
   * `ND+0x278`/`+0x27c` are SMPTE/address-source settings **(uncertain)**.
4. When the hold expires, `ND424::HandleInterrupt` @0x2ada0 (it subtracts `n*samples-per-interrupt` each period) clears `ND+0x210` and calls (C) and (D) again, this time with the real `kModes[ws]` and one-x bits.

### 5.3 Full sample-rate change (`Drv::SetSampleRate` @0x12990)

1. Return early if the rate is unchanged or not offered (section 2.2).
2. If the clock source `Drv+0x3d2c` is a wire clock (> 7):
   1. Set the card word source to 0.
   2. Tell every connected box to follow the card (`SetWordClockSourceToCard`, slot 0xb8).
   3. Sleep 500 ms. The previous source is restored at the end through `Drv::SetClockSource` @0xfbf0.
3. Compute `mult` (1 if ≤ 48000, 2 if ≤ 96000, otherwise 4) and `base = rate / mult`.
4. If `mult` changed:
   1. `Drv+0x40 = mult`.
   2. Free the cue-mix routings and wait for MIDI to drain.
   3. For each wire with an interface that supports `mult`: clear its MIDI busy counter and call `SetSampleRateMultiplier(mult)` on the interface. This sends SysEx to the box.
   4. Wait for MIDI to drain, then sleep 100 ms.
   5. `Drv+0x44 = !card_supports_in_hw(mult)`. This is true only at 4x.
   6. `wire_mult = Drv+0x44 ? mult/2 : mult`, and `Drv+0x45 = (wire_mult == 2)`.
   7. `mask` bit w is set when `Drv+0x45` is set and wire w has no interface or its interface lacks `SupportsSampleRateMultiplierInHardware(wire_mult)`.
   8. Call `Drv424::SetSampleRateMultiplier(mult, mask)` (event B). It also sets the mixer fader count: 0xb4 at 1x, `Drv+0x20ca0` at 2x, half of that at 4x.
5. `SetBaseSampleRate(base)` (event A). Then every interface gets `SetSampleRate(base*mult, family_changed)` (slot 0xc0).

**Linux status:** rate changes are made from PCM `hw_params`. On the tested
PCIe-424, repeated 44.1/48 kHz family changes and 88.2/96 kHz loopbacks have
passed with a 24I/O; see the [user guide](USER_GUIDE.md) for the test scope.
The 4x path, other card variants and interactions with external clock sources
remain untested on hardware.
* Within a multiplier: event A (`cb+0x98`, then `cb+0x114 = 1`), then a 1296/HD192 state resend for its rate-dependent AES output clock.
* A multiplier change (`motu424_change_mult`) follows step 4 with these differences:
  * With a wire clock, only the card is switched to internal; the boxes are not told to follow the card.
  * The engine runs idle during steps 4.2–4.4 so that the MIDI drains; it is stopped for event B.
  * The Altera settle timer is a 40000-frame sleep instead of a per-period countdown.
  * The fader budget is 180/96/48 on the PCI-424 and 180/180/90 on the C6412 cards. The PCI-424X is assumed to be C6412-class **(uncertain)**.
* Above 1x, a present-mask change restarts the settle timer, as in `SetDevicePresentMask`. So does a change of the wire mask after a box is identified.
6. Update the SMPTE generator.

### 5.4 Multiplier summary

| Rate | mult | Card (`Drv+0x44`) | Wire framing | cb+0x98 | cb+0x80 Altera part |
|---|---|---|---|---|---|
| 44100 | 1 | native | 1x | 0x05ACBF46 | 0x80000000 |
| 48000 | 1 | native | 1x | 0x05355555 | 0x80000000 |
| 88200 | 2 | native | 2x on native wires; doubled on masked wires | 0x05ACBF46 | 0x80001000 \| mask<<24 \| ~mask<<20 |
| 96000 | 2 | native | (same as 88200) | 0x05355555 | (same as 88200) |
| 176400 | 4 | card doubles (`Drv+0x44`) | 2x (HD192 only) | 0x05ACBF46 | (same as 88200) |
| 192000 | 4 | card doubles | 2x (HD192 only) | 0x05355555 | (same as 88200) |

`ND424::SetWireRateMultiplier(w, m)` @0x2ab20 changes a single wire's mask bit: the bit is set when m ≤ 1 and cleared when m > 1. It then rewrites `cb+0x80` with a new settle timer.

---

## 6. Interrupt period (`cb+0x60` / `cb+0x64`)

* `ND424::SetSamplesPerInterrupt(n)` @0x2a5e0 writes `cb+0x60 = 16 << (2n)`. This is a **frame count** (16, 64, 256 or 1024) and not a logarithm. The value is also kept in `ND+0x280`.
* `ND424::SetInterruptsPerBuffer(k)` @0x2a610 writes `cb+0x64 = k`.
* Only two places call them (through vtable slots 0x130 and 0x140), and both always pass **n = 3 and k = 3**: 1024 frames per interrupt and 3 interrupts per buffer.
  * `Drv::StartIdling` @0x139c0 does so for idle mode, where only status and MIDI are transferred and there are no audio channels.
  * `Drv::StartAudioRunning` @0x13a80 does so for streaming.
* In both cases they are followed by:
  1. `ZeroOutputDMAArea` (slot 0x148);
  2. `SetupInterruptRelatedMembers(nOutPrimary, nInPrimary, nOutTotal, nInTotal, 0x40, 1, 3)`, where 0x40 = 64 sixteen-frame blocks per period;
  3. `SetInterruptsEnabled(1)`.
* The vendor period is the same at every rate. The kext never uses other values of n.

The Linux driver exposes all four settings, keeping the ring at three
periods. On a PCIe-424 at 48 kHz, full-duplex analog loopback worked at 16,
64, 256 and 1024 frames. The 16-frame setting passed a two-minute loaded
test; thus the supplied PCIe-424 firmware accepts it on that setup. Other
card variants and higher rates remain untested. At 4x rates each hardware
block represents 32 ALSA frames instead of 16, so ALSA period sizes double.
See the [driver latency results](../linux/snd-motu424/README.md#pcm-latency-test).
