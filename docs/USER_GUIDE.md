# snd-motu424 user guide

**Experimental.** Hardware testing has covered one setup: a PCIe-424 with a 24I/O, a 2408, a 2408mk2 and a 24i, running x86_64 Linux 6.14 and Ubuntu 24.04 PREEMPT_RT (`6.8.1-1015-realtime`).

| Area | State |
|---|---|
| Boot (FPGA, DSP firmware), interrupts | Works on the PCIe-424 |
| Interface detection and clock setup | Works for the 24I/O, 2408, 2408mk2 and 24i, also with all four connected at once |
| Channel layout from the detected boxes | Works, including a box that is switched off and on again |
| Capture, 48 kHz | Works. The inputs of all four boxes arrive on the expected channels (96 in total) |
| Playback, 48 kHz | Works. Checked by analog loopback on the 24I/O (six channels: no dropouts or discontinuities), on the 24i and on the 2408mk2 (one channel each). The outputs of the 2408 and all digital outputs have not been checked |
| Q-Mix input monitoring and input trim/mute | Two 24i inputs were loopback-tested at 48 kHz for routing, pan, solo and mute; trim was tested on one. Other inputs and idle monitoring remain untested |
| Q-Mix stereo balance/width, talkback/listenback and DSP meters | Implemented; selected routes and meter exchange checked at 48 kHz with capture active (see below) |
| Small PCM periods | 16, 64 and 256 frames checked at 48 kHz; lowest measured application round trip 3.46 ms with a 16-frame period (see latency test). Two-period buffers and non-power-of-two periods (48, 96) passed loopback |
| PipeWire 1.0.5, JACK2 1.9.21, pipewire-jack | Pro-audio and stereo profiles, quanta 64–2048, 44.1/48/96 kHz, jackd `-n 2`/`-n 3` with periods 32–1024: all passed silent tests and a channel-19 loopback (see "Sound servers") |
| 44.1 kHz, switching between 44.1 and 48 kHz while loaded | Repeated 24I/O loopback and rate changes passed |
| 88.2/96 kHz (2408mk3, 24I/O, 1296, HD192) and 176.4/192 kHz (HD192 only) | 88.2/96 kHz channel-19 loopbacks passed on the 24I/O; 96 kHz also passed 120 seconds loaded at 16-frame periods. Other rates and devices are untested |
| ALSA rawMIDI | Four duplex AudioWire-port substreams; not hardware-tested |
| PCI-424 (`137a:0003`), PCI-424X (`137a:0004`) | Not tested |
| 2408 bank selection, including 2408mk3 | ALSA controls and channel-layout changes implemented; not hardware-tested |
| 1296, HD192 | Clock setup and 12-in/12-out layout ported from the Mac and Windows drivers; not tested (no hardware available) |
| SMPTE (LTC) reader and generator, video sync, C6412 S/MUX option | Ported from the Mac driver. On a PCIe-424, the controls load, source validation works and rate changes reprogram the parameters. No LTC or video signal has been read or generated |

The driver builds with `W=1` against Linux 6.1 and 6.12 ARM64 headers and Linux 6.14 x86_64 headers. It also built and loaded on Ubuntu 24.04 with kernel `6.8.1-1015-realtime`. A two-channel S32_LE capture at 48 kHz opened and completed on that system; this smoke test did not check the audio signal or a loopback. See [HARDWARE.md](HARDWARE.md) for register-level behavior and [INTERFACES.md](INTERFACES.md) for the interface protocol.

## 1. Extract the firmware (from your own MOTU installer)

Run this command from the repository root:

```sh
python3 tools/extract_firmware.py -o /tmp/fw "MOTU Audio Installer 1.6 (73220).pkg"
sudo mkdir -p /lib/firmware/motu && sudo cp /tmp/fw/* /lib/firmware/motu/
```

MOTU's firmware is not redistributable, so it is not included in this repository.

## 2. Build and install

Run these commands from the repository root. The build uses headers for the
running kernel:

```sh
make -C linux/snd-motu424
sudo make -C linux/snd-motu424 install
sudo modprobe snd_motu424
```

For a one-off test without installing the module, load the built file with
`sudo insmod linux/snd-motu424/snd-motu424.ko`. The optional `boot_only=1`
parameter boots the card but intentionally registers no PCM device; it is a
diagnostic mode, not the normal way to start the driver. To switch from a
boot-only load to normal operation, stop audio clients and unload it first.
For an installed module, run `sudo modprobe -r snd_motu424` followed by
`sudo modprobe snd_motu424`. For a one-off `insmod` load, run
`sudo rmmod snd_motu424` followed by the normal `insmod` command above.

## Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `boot_only` | 0 | Diagnostic mode: boot DSP and FPGA and dump the control block, but register no PCM. |
| `trace_boot` | 0 | Verbose boot log (each FPGA/DSP step) |
| `trace_midi` | 0 | Log the SysEx messages exchanged with the interfaces (can be changed at runtime in `/sys/module/snd_motu424/parameters/`) |
| `fpga_delays` | 1 | Add small µs delays to FPGA bit-banging (MOTU uses none) |
| `rate` | 48000 | Initial rate, 44100 or 48000; streams can change it (see below) |
| `index`, `id`, `enable` | | Standard ALSA parameters |

## Interface clock setup

The card starts on its internal clock at the rate set by `rate`. Boxes do not lock to the card automatically. After power-up, they have no valid clock source; on a 24I/O, the 44.1 kHz LED flashes. On module load, the driver follows MOTU's setup sequence:

1. The DMA engine starts in idle mode, without audio channels. Its header and status slots carry a MIDI link for each AudioWire port.
2. For each port that reports a box, the driver sends a MIDI Device Inquiry and identifies the box from its reply.
3. The driver sends a state message that sets the box's clock to "follow the card", then reads the state back and checks it.
   * The 24I/O, 2408, 2408mk2 and 2408mk3 use fixed messages. A 2408 is set to analog bank A and ADAT optical on banks B and C.
   * Self-describing boxes, such as the 24i, report their bank and clock-source options. The driver reads those lists and selects the "card" clock source.
4. On module unload, the driver sends each box the same release message as MOTU's driver.

Run `cat /proc/asound/M424/motu424` to see the engine mode, interrupt count and port states. A configured port is shown as, for example, `Port 0: 24I/O, configured`. Self-describing boxes also report their banks.

## Experimental external clocking

The `Card Clock Source` values are:

* `0`: internal clock.
* `2`: the card's ADAT sync input.
* `3`: SMPTE/LTC (see [SMPTE timecode](#smpte-timecode-video-sync-and-smux)).
* `4`–`7`: video sync from a 2408mk3 on AudioWire port 0–3. The control returns
  `ENODEV` if the selected port has no 2408mk3.
* `8`–`11`: AudioWire ports 0–3.

The `rate` setting remains the expected card rate. The driver does not detect
clock lock or the external source's rate, so open streams at the rate that
source is running. During a change between 1x, 2x and 4x, the card temporarily
switches to its internal clock, then returns to the selected source.

The driver exposes AudioWire clock sources for 2408-family boxes and the
1296/HD192. Set the box's clock source first, then select its port in
`Card Clock Source`:

* `Interface 2408 Clock Source`: 0 is Word Clock In, 1–3 are banks A–C optical,
  4 is S/PDIF, and 5 follows the card (the default).
* `Interface 1296 Clock Source`: 0 is the box's internal clock, 1 Word Clock In,
  2 AES/EBU, and 3 follows the card (the default). The internal clock code
  follows the card's rate family. An HD192 at 4x has no AES clock and runs
  on its internal clock instead.

For example, to use bank A optical from the 2408 on port 0:

```sh
amixer -c M424 cset "name='Interface 2408 Clock Source',index=0" 1
amixer -c M424 cset "name='Card Clock Source',index=0" 8
```

The driver rejects a box clock-source change while PCM is open or while the
card already follows that port. It also rejects card clock changes while PCM
is open. This order avoids a feedback loop, but does not confirm that an
external signal is present or running at the expected rate. External clocking
has not been tested on hardware.

## Sample rate

Applications select 44.1, 48, 88.2, 96, 176.4 or 192 kHz through ALSA hardware
parameters. No separate rate control or module reload is needed. The capture
and playback directions share the card clock: the first direction to set its
hardware parameters selects the rate, and the other direction must use the
same rate. ALSA rejects a conflicting exact-rate request during parameter
negotiation; a near-rate request may be adjusted to the rate already in use.
Once neither direction holds hardware parameters, another rate can be
selected, even if a PCM handle remains open. The current rate appears in
`/proc/asound/M424/motu424`.

The driver changes the card's rate-family constant, as MOTU's driver does.
The 24I/O, 2408 and 24i follow the card automatically. The 1296/HD192 receives
a new state message because its AES output clock depends on the card rate.
With an external clock selected, the driver records the requested rate and
logs a warning; it cannot change the external source.

**Higher rates are only partially tested on hardware.** The rate-change
sequence follows MOTU's Mac driver (`Drv::SetSampleRate`). A switch between
1x (44.1/48 kHz), 2x (88.2/96 kHz) and 4x (176.4/192 kHz) takes about a
second. The driver releases Q-Mix routes, updates each box that supports the
new multiplier, reconfigures AudioWire framing and restores the routes within
the new fader budget. The budget is 180 routes at 1x; at 2x and 4x it is 96
and 48 on PCI-424, or 180 and 90 on other cards. If too many routes are
unmuted, the rate change fails with `ENOSPC`; mute routes before switching.

On a 24I/O, a 24-channel 96 kHz capture ran without an xrun, and a 30-second
channel-19 analog loopback passed with 24 capture and 24 playback channels.
The first full-duplex stream after a multiplier change initially failed. A
one-second wait during the first PCM prepare fixed immediate 48-to-96 and
96-to-48 kHz loopbacks. Other PCM setup waits for that prepare to finish. This
wait is empirical; the driver does not detect when the interfaces have
settled. Other channel assignments and 4x rates still need testing.

Direct 88.2 and 96 kHz loopbacks and repeated switching between those rates
also passed. At 96 kHz, a 120-second run with 16-frame periods, a 48-frame
ALSA buffer, two playback periods queued, an SCHED_FIFO 40 application and
four nice-10 CPU workers completed without xruns, weak correlation windows
or bad chip centers. The measured application round trip was 1.729 ms.

| Box | 2x | 4x |
|---|---|---|
| 2408mk3 | analog bank 8 channels; each ADAT/TDIF bank 4 (S/MUX), S/PDIF 2 | channels hidden |
| 24I/O | 24 channels **(uncertain: may be 12)** | channels hidden |
| 1296 | 12 channels; the card doubles the port | channels hidden |
| HD192 | 12 channels | 12 channels; the card doubles the port |
| 2408, 2408mk2, 24i and other boxes | channels hidden | channels hidden |

A box that does not support a rate stays configured at its last rate and its
channels disappear from the PCM device; they come back when the rate returns.
A stream can only use a rate at which the configured boxes give it channels.
The number of channels the boxes give depends on the rate (for example a
2408mk3 has 24 channels at 48 kHz and 16 at 96 kHz), but a stream may open
any channel count up to the largest at any rate (see "Channels"), so a sound
server can keep its channel count when it changes the rate. At 176.4/192 kHz
periods are multiples of 32 frames instead of 16.

## Channels

PCM channels follow port order, starting at port 0, and each box's own
channel order. Capture and playback counts are separate: a 24i adds 24 capture
channels and two playback channels. For example, with a 24I/O, a 2408, a 24i
and a second 2408 on the four ports:

```
Capture: 96 channels
  1-24: port 0, 24I/O 1-24
  25-48: port 1, 2408 1-24
  49-72: port 2, 24i 1-24
  73-96: port 3, 2408 1-24
Playback: 74 channels
  1-24: port 0, 24I/O 1-24
  25-48: port 1, 2408 1-24
  49-50: port 2, 24i 1-2
  51-74: port 3, 2408 1-24
```

The channel list appears at the end of `/proc/asound/M424/motu424`. A 2408's
24 channels are bank A (analog 1–8), followed by banks B and C (ADAT 1–8
each).

* A stream can request between 1 and the largest channel count available at
  any rate (96 capture and 74 playback in the example). It uses the first
  channels in the list. Unused playback channels are silent; channels that
  are unavailable at the current rate also read as silence.
* The layout follows the connected boxes. Turning a box off removes its
  channels and shifts the following channels forward. Open streams keep their
  original layout until all streams close.
* The driver configures connected boxes before registering the sound card, so
  loading takes a few seconds. The PCM device cannot be opened if no box is
  configured.
* Playback channels are exposed in stereo pairs, as in MOTU's driver. A
  single unpaired output is omitted.

## MIDI ports

The card exposes one duplex ALSA rawMIDI substream per AudioWire port.
Subdevices 0–3 map to ports 0–3. Run `amidi -l` to list them, then select the
appropriate subdevice in your MIDI application. For example, send a note on
port 0 with:

```sh
amidi -p hw:M424,0,0 -S "90 3C 7F"
```

The driver consumes MOTU interface-control SysEx and Universal Device Inquiry
replies. Other received MIDI, including unrelated SysEx, is passed to the
matching rawMIDI input. Output is queued until the box is configured and the
driver has finished sending its own control messages, then transmitted at the
AudioWire MIDI rate. MIDI round-trip has not been tested on hardware.

## Q-Mix controls

The optional [Q-Mix browser interface](../linux/snd-motu424/qmix-ui/README.md)
runs as a user-space service and controls the ALSA mixer controls described
below. It does not add networking to the kernel driver.

Q-Mix routes physical inputs to stereo output pairs without host playback.
Selected routes have passed loopback tests while capture was active. **Audible
monitoring with PCM idle has not been tested.** Keep external monitors muted
during initial testing; enabling a route and its bus can make a signal audible.

Controls use fixed, zero-based hardware indices rather than the compact PCM
channel numbers. An input index is `port * 24 + input within port` (0–23); a
bus index is `port * 12 + output pair within port` (0–11). Bus 0 is port 0
outputs 1–2, and input 95 is port 3 input 24. Controls are also present for
absent ports and channels.

| ALSA control | Index | Values |
|---|---:|---|
| `Q-Mix Input Trim Volume` | input 0–95 | 64–255; 64 is unity |
| `Q-Mix Input Switch` | input 0–95 | off/on |
| `Q-Mix Bus N Input Playback Volume` | input 0–95 | 0–32768 |
| `Q-Mix Bus N Input Playback Pan` | input 0–95 | 0–128; 64 is center |
| `Q-Mix Bus N Input Playback Switch` | input 0–95 | off/on |
| `Q-Mix Bus N Input Solo Switch` | input 0–95 | off/on |
| `Q-Mix Bus Playback Volume` | bus 0–47 | 0–32768 |
| `Q-Mix Bus Playback Switch` | bus 0–47 | off/on |
| `Q-Mix Bus N Stereo Balance` | input pair 0–47 | 0–128; 64 is center |
| `Q-Mix Bus N Stereo Width` | input pair 0–47 | 0 = stereo, 64 = mono, 128 = reversed |
| `Q-Mix Talkback Switch`, `Q-Mix Listenback Switch` | 0 | off/on |
| `Q-Mix Talkback Input`, `Q-Mix Listenback Input` | 0 | physical input 0–95; 96 = none |
| `Q-Mix Talkback Dim`, `Q-Mix Listenback Dim` | 0 | 0–128; 128 = no dimming |
| `Q-Mix Talkback Bus Switch`, `Q-Mix Listenback Bus Switch` | bus 0–47 | off/on |
| `Q-Mix Meter Input Bank` | 0 | current input bank; successful peak reads alternate banks |
| `Q-Mix Meter Bus` | 0 | bus 0–47; 48 = output meters disabled |
| `Q-Mix DSP Peak` | 0 | read-only volatile array of 52 raw peaks, 0–65535 |

Input trim defaults to 64 and its switch to on. Route and bus switches default
to off; both volumes default to 32768. Route and bus controls affect **input
monitoring only**. The bus switch and volume do not mute or attenuate host PCM
playback. Input trim and mute are global DSP settings, not per-bus controls.
On one 24i input at 48 kHz, raising trim from 64 to 128 increased captured and
monitored noise by about 6 dB. Turning the input switch off silenced both.
Settings reset when the module is reloaded.

At 4x rates, an available input's trim and switch also apply to its hardware
partner, 12 slots later on the same port. The partner's stored settings resume
at other rates. A trim write queues a DSP update; readback reports the
requested value, not the confirmed DSP state. A later change to the effective
factors waits for the pending update and returns `ETIMEDOUT` if it does not
complete. Check the kernel log and reload the driver if the update remains
stuck.

With monitors muted, these commands route port 0 input 1 to port 0 outputs 1–2
at reduced route and bus gains. The bus is enabled last:

```sh
amixer -c M424 cset "name='Q-Mix Bus 0 Input Playback Volume',index=0" 8192
amixer -c M424 cset "name='Q-Mix Bus 0 Input Playback Pan',index=0" 64
amixer -c M424 cset "name='Q-Mix Bus Playback Volume',index=0" 8192
amixer -c M424 cset "name='Q-Mix Bus 0 Input Playback Switch',index=0" 1
amixer -c M424 cset "name='Q-Mix Bus Playback Switch',index=0" 1
```

An eligible solo-marked input suppresses non-soloed inputs on that bus, even
when the soloed route is switched off. Clear the solo switch when finished.
The driver reserves one of 180 fader slots for every switched-on input route
on a switched-on bus. This includes unavailable routes, solo-suppressed routes
and forced talkback/listenback routes. Changes that exceed the limit fail with
`ENOSPC`; an oversized DSP program can also return `ENOSPC`.

When an interface disappears, the driver disables its Q-Mix routes. Previously
enabled routes can reactivate when the interface is ready again. While PCM is
open, monitoring is limited to available channels in the stream's frozen
layout; newly added channels become eligible after all streams close. Playback
DMA keeps its original slot mapping. If the driver reports `Q-Mix DSP state
uncertain`, hardware state may not match control readback. Mute external
monitors and reload the driver before changing routes.

### Stereo, talkback/listenback and metering

Stereo pair `p` on bus `N` uses physical inputs `2*p` and `2*p+1`. As in the
Mac application, a stereo adjustment **overwrites both mono routes' volume
and pan**, using the greater of their previous volumes. Balance pans the
routes hard left and right, attenuating the opposite side; width sets the pans
to `w` and `128-w`.

Both stereo controls initially read 64. Readback reports only the last
requested stereo value; it does not indicate a stereo link or derive a value
from the current mono settings. Mono edits remain independent, and writing the
same stereo value applies the transform again. Route mute and solo are also
independent. At 48 kHz, width 0/64/128 routed a 24i pair to 24I/O outputs
19–20 as normal, mono and reversed. Balance 128 suppressed the left input.

Talkback and listenback each have an input selector, an active switch, a dim
value and destination-bus switches. They start with no input or destination,
are inactive and do not dim other routes. An active microphone is sent to its
selected, unmuted buses even if its ordinary route is muted or excluded by
solo. Its route volume and pan, plus global input trim and mute, still apply.

For other routes on those buses, the driver multiplies route volume by
`dim/128` **before** the nonlinear gain calculation. A value of 0 silences
those routes; 128 leaves them unchanged. A value of 64 is not calibrated to
-6 dB. When both functions are active on a bus, talkback dim takes precedence
for non-talkback inputs. Host PCM playback is not dimmed. Tests with 24i inputs
1 and 14 and 24I/O outputs 19–20 confirmed forced microphone routing,
dim-to-zero and restoration for each function.

Reading `Q-Mix DSP Peak` swaps the DSP peak buffers. The first read primes the
selection. Each successful read then returns peaks for the current physical
input bank and selects the other bank for the next interval. Read
`Q-Mix Meter Input Bank` immediately before the peak array to identify which
bank is represented by values 0–47; the read advances the bank.

Values 48–51 are the selected bus's pre-left, pre-right, post-left and
post-right peaks. "Pre" includes host PCM playback; "post" also includes
ordinary Q-Mix routes. These are **raw DSP values, not dBFS-calibrated
meters**. The driver does not apply the vendor application's gain correction
or display compression. Unavailable inputs and disabled output meters return
zero. Selecting an output bus adds DSP meter instructions and can fail with
`ENOSPC`. The first read after a selection or topology change may wait 20 ms
to prime the buffer. A timed-out bank exchange reports an error; if completion
is uncertain, further reads fail until the module is reloaded.

While a meter stream is connected, the web service reads meters at a target
60 Hz and shares samples with all browsers. At that rate, each input bank is
sampled about 30 times per second. Do not read `Q-Mix DSP Peak` from another
process while the web stream is active; each read consumes the accumulated
peak window. Hardware tests confirmed bank exchange and nonzero right-channel
output readings. Automatic bank attribution still needs checking with known
live signals.

## 1296/HD192 interface controls

Each setting is an ALSA mixer control with one element per AudioWire port
(`index=0`–`3`). These controls apply only to a 1296 or HD192. On other ports,
the element is inactive and reads 0; writes return `ENODEV`. The active flags
follow interface arrival, identification and removal.

A write queues a SysEx update and returns before the box confirms it. Further
writes to that port return `EBUSY` until the exchange completes. Readback
shows the requested value, not confirmed hardware state. Check
`/proc/asound/M424/motu424` and the kernel log. If retries are exhausted, the
port becomes unconfigured and controls remain unavailable until the box is
identified again. These settings and their audible effects have not been
tested on hardware.

| ALSA control | Values | Meaning |
|---|---:|---|
| `Interface AES Input Pair` | 0–6 | 0 disables AES input replacement; 1–6 selects the input pair |
| `Interface AES Output SRC Mode` | 0–5 or 8–13 | MOTU mode code; 6 and 7 are rejected |
| `Interface AES Output Clock` | 0–10 | 0–2 are special modes; 3–10 select 44.1/48/88.2/96 kHz options |
| `Interface AES Input SRC` | off/on | Enable sample-rate conversion for the AES input |
| `Interface Peak Hold Time` | 0–7 | Peak-meter hold setting |
| `Interface Clip Hold Time` | 0–7 | Clip-indicator hold setting |
| `Interface 1296 Clock Source` | 0–3 | Internal, word clock, AES/EBU, follow the card (see "Experimental external clocking") |

The driver preserves the two SRC Mode code ranges and hold-time scales from
MOTU's driver; their individual meanings are unknown. For output clock, it
uses the card's current rate to calculate the transmitted code, following
MOTU's single-speed logic. When detected, the box settings reset to MOTU's
defaults: input pair 0, output mode 0, input SRC off, peak hold 4 and clip
hold 1. For example:

```sh
amixer -c M424 cset "name='Interface AES Input Pair',index=0" 1
```

## SMPTE timecode, video sync and S/MUX

The card can read LTC from an analog input or a 2408mk3 SMPTE jack, use it as
a clock source, and generate LTC on an output. ALSA controls cover the
functions of MOTU's "MOTU PCI SMPTE Setup" app. See [SMPTE.md](SMPTE.md) for
the protocol. No real LTC or video signal has been tested.

Source and destination codes use this layout:

* `0`: none.
* `24 * port + channel`: channel 1–24 on AudioWire ports 0–3. For example,
  `1` is port 0 channel 1 and `25` is port 1 channel 1.
* `97`–`100`: the dedicated SMPTE jack on a 2408mk3 at port 0–3.

A reader source must be an analog input, matching MOTU's source list. The
driver rejects a code that is unavailable in the current topology with
`EINVAL`. If a selected source or destination disappears because a box is
removed, a bank changes or the rate changes, the control reads 0. The setting
returns when the channel becomes available again.

| ALSA control | Values | Meaning |
|---|---:|---|
| `SMPTE Frame Format` | 0–4 | 24, 25, 29.97 drop, 29.97 non-drop, 30 fps (default 30) |
| `SMPTE Format Follow Switch` | off/on | Switch the format to the detected LTC format (default on; 30 fps detection selects the last 29.97 ND/30 choice) |
| `SMPTE Detected Format` | 0–4, read-only | 0 none yet, then 24, 25, 29.97 drop, 30 (29.97 non-drop cannot be told from 30) |
| `SMPTE Reader Source` | code | LTC input |
| `SMPTE Reader State` | 0–6, read-only | MOTU's transport state; 0 when the card does not clock to LTC or video, 1 no source or no signal (other values uncertain) |
| `SMPTE Reader Status` | 2 values, read-only | Raw DSP reader and clock states (`cb+0x34`, `cb+0x38`) |
| `SMPTE Freewheel Address Time`, `SMPTE Freewheel Clock Time` | −1 – 3600000 ms | How long to coast over LTC dropouts; −1 forever, default 0 |
| `SMPTE Generator Destination` | code | LTC output |
| `SMPTE Generator Level` | 0–65535 | Output level for a channel destination (default 32767; not used for the 2408mk3 jack) |
| `SMPTE Generator Start Time` | h, m, s, f | Start timecode, validated against the frame format |
| `SMPTE Generator Switch` | off/on | Start or stop generating from the start time |
| `SMPTE Generator Time` | h, m, s, f, read-only | The frame being generated |
| `SMPTE Generator Audio Overwritten` | off/on | Set when the generator overwrote audio on its output; write 0 to clear |
| `Card S/MUX Type` | 0–1 | PCI-424X/PCIe-424 only: 0 Type II (default), 1 Type I for the ADAT banks of a 2408mk3 |

To clock the card to LTC on 24I/O input 1 (port 0), with 25 fps:

```sh
amixer -c M424 cset name='SMPTE Frame Format' 1
amixer -c M424 cset name='SMPTE Reader Source' 1
amixer -c M424 cset name='Card Clock Source' 3
```

To generate LTC from 01:00:00:00 on output 3 of the box on port 1, at a low level:

```sh
amixer -c M424 cset name='SMPTE Generator Level' 2000
amixer -c M424 cset name='SMPTE Generator Destination' 27
amixer -c M424 cset name='SMPTE Generator Start Time' 1,0,0,0
amixer -c M424 cset name='SMPTE Generator Switch' on
```

The generator replaces any audio played or mixed on its output. Only MOTU's
manual start mode is implemented; chase and scheduled start are not. For
video or SMPTE clocking, or an LTC source on a 2408mk3, the driver sets the
box's video and LTC state bits. Selecting the dedicated jack enables the
box's SMPTE output. Status controls are polled every 100 ms.

## 2408 bank controls

The 2408 and 2408mk3 bank controls are indexed by AudioWire port. On other
boxes, they are inactive and read 0.

`Interface 2408 Analog Bank` selects A, B or C with values 0, 1 or 2. Value 3
selects no analog bank: all three banks become digital, and the analog outputs
mirror the bank selected by `Interface 2408 Analog Mirror Bank` (0/1/2 = A/B/C).
The mirror setting is ignored while a bank is selected as analog.

`Interface 2408 Bank A Format` and `Bank B Format` use 0 for ADAT and 1 for
TDIF. `Interface 2408 Bank C Format` uses 0 for ADAT, 1 for TDIF and 2 for
S/PDIF. Bank C cannot use S/PDIF while it is the analog bank; before moving
the analog bank to C, change C from S/PDIF.

Writes queue a SysEx update and return before the box confirms it. Wait until
the port is `configured` in `/proc/asound/M424/motu424` before writing again;
the controls return `EBUSY` while an update is pending. A bank change also
returns `EBUSY` while PCM is open because it changes the channel layout. After
confirmation, the driver rebuilds the capture/playback channel counts and
hardware mappings. Readback shows the requested settings, not confirmed
hardware state. A 2408 echoed the no-analog-bank and mirror settings, but the
bank audio paths have not been tested on hardware.

For example, set port 0's analog bank to B, then set bank C to S/PDIF:

```sh
amixer -c M424 cset "name='Interface 2408 Analog Bank',index=0" 1
amixer -c M424 cset "name='Interface 2408 Bank C Format',index=0" 2
```

## Input reference levels and word clock out

These controls expose box options from MOTU's setup application. They are
indexed by AudioWire port and do not change the channel layout, so they can be
changed while streaming. Each defaults to off or 0 when the box is detected.

| ALSA control | Box | Meaning |
|---|---|---|
| `Interface 24I/O Analog 1-8 -10dBV Switch` (also `9-16`, `17-24`) | 24I/O | on = −10 dBV, off = +4 dBu for that group of eight analog inputs |
| `Interface 2408mk3 Analog 1-2 -10dBV Switch` (also `3-4`, `5-6`, `7-8`) | 2408mk3 | the same per input pair |
| `Interface Word Clock Out Rate` | 24I/O, 2408mk3 | 0 = match the system rate, 1 = always 44.1/48k, 2 = always 88.2/96k |

PCI-424 boxes do not provide phantom power, preamp gain or pad controls. On
hardware, the 24I/O reference-level switch measured about +13.6 dB for an
input set to −10 dBV. The 2408mk3 settings have not been tested.

```sh
amixer -c M424 cset "name='Interface 24I/O Analog 17-24 -10dBV Switch',index=0" on
```

## Current limitations

* ALSA periods can be any multiple of 16 frames, up to 8192 frames (32-frame
  multiples at 176.4/192 kHz). At least two periods are required, and the
  buffer can be no larger than 16384 frames. The card interrupts every 16, 64,
  256 or 1024 frames, doubled at 4x, using a ring of three engine periods. The
  driver chooses the largest interrupt period that divides the ALSA period. If
  both directions are configured before the engine starts, it also considers
  the other direction's period. A stream opened while the other direction is
  running must use a multiple of the active engine period.
* The PCM pointer advances in engine-period steps, and the device is marked
  `BATCH`. Playback reports two engine periods of delay for data queued in the
  DMA ring.
* Each direction exposes one block of channels, without channel names or
  separate PCM devices per box. A stream with fewer channels uses the first
  channels in the block; it cannot select a different subset.
* Sound servers that keep the device open retain the old channel layout after
  a box is added or removed. Close and reopen the device to refresh it. In
  PipeWire, switch the profile off and on again.
* PipeWire versions before 1.6 support at most 64 channels per node. A
  pro-audio profile with more than 64 inputs or outputs exposes only the first
  64. JACK and PipeWire 1.6 or later support 128 channels and can expose them
  all.
* External clock selection is experimental; the driver does not detect lock
  or source rate. SMPTE/LTC and video clocking, LTC generation and the C6412
  S/MUX "Type I" option are implemented from the Mac driver but have not been
  tested with a signal. Some DSP register meanings are inferred; see
  [SMPTE.md](SMPTE.md) §8 and [INTERFACES.md](INTERFACES.md) §2.3.
* The 24I/O passed 88.2/96 kHz channel-19 loopback tests. Other 2x channel
  mappings and 4x rates are untested. Unverified details ported from the Mac
  driver include other 24I/O mappings at 2x, 2408mk3 S/MUX slot mapping, 4x
  sample interleave, the PCI-424X fader budget and the meaning of the card's
  AudioWire "settled" bits.
* A 1296 or HD192 is set to follow the card with MOTU's default AES/EBU
  settings. ALSA controls expose input pair, input SRC, output SRC mode,
  output clock and meter hold settings, but none have been tested on hardware.
  The box clock source is also selectable with `Interface 1296 Clock Source`;
  that setting is untested as well.
* Q-Mix meters report raw DSP peaks. Persistent stereo linking, a combined
  talkback/listenback switch and monitor dimming for host playback are not
  implemented.
* PCI-324 is not supported; see [HARDWARE.md](HARDWARE.md) §9.

## PCM latency test

Run these commands from `linux/snd-motu424/`.

`tools/latency_test.c` requires ALSA development headers and `libasound`. It
opens linked full-duplex streams at the requested rate, period and buffer, and
does not recover from xruns. The selected output carries a -40 dBFS peak test
signal; all other playback channels are silent. **Mute external monitors and
disable Q-Mix routes before testing.** Connect the selected output to the
selected input. Use the full channel counts and indices for your hardware; the
example connects 24I/O output 19 to input 19:

```sh
cc -O2 -Wall -Wextra -Werror tools/latency_test.c -lasound -lm -o /tmp/motu424-latency-test
/tmp/motu424-latency-test --selftest
# DEVICE RATE PERIOD BUFFER QUEUE_PERIODS SECONDS OUT_CHANNELS IN_CHANNELS OUT IN
/tmp/motu424-latency-test hw:M424,0 48000 16 48 2 120 74 96 18 18
```

For rate-switch diagnostics, set `MOTU424_START_DELAY_MS`. The test waits
after preparing both streams and filling the initial playback queue, then
starts them together.

With the four-box setup above on Linux 6.14, normal (non-realtime) scheduling:

| Period (frames) | ALSA buffer (frames) | Playback queue (periods) | Application round trip |
|---:|---:|---:|---:|
| 1024 | 3072 | 2 | 87.46 ms |
| 256 | 768 | 2 | 23.46 ms |
| 64 | 192 | 2 | 7.46 ms |
| 16 | 48 | 2 | **3.46 ms** |

All four period/buffer combinations passed 10-second comparisons. The
16-frame, two-period queue also passed a 120-second run with four nice-10 CPU
workers on a 16-logical-CPU machine, followed by 60 seconds without added
load. Both runs had zero xruns, no 512-frame correlation windows below 0.90,
and no low or wrong-polarity chip-centre samples. The loaded run's minimum
window correlation was 0.9677. These finite runs do not guarantee performance
with every workload or sound server. A three-period queue at 16 frames
measured 3.79 ms and passed a 20-second loaded test. A one-period queue at 16
or 64 frames xran immediately.

On the PREEMPT_RT kernel, two-period ALSA buffers passed 15-second loopbacks
with the application at SCHED_FIFO 70. Tested at 48 kHz: 48/96, 128/256,
256/512 and 1024/2048 frames, plus a 96-frame period with a 288-frame buffer.
At 96 kHz, 128/256 also passed. For a two-period buffer, the playback queue
must be 2 (the full buffer). Periods of 48 and 96 frames run the engine at 16
frames and measured the same correlation lag as 16/48.

On Ubuntu 24.04's PREEMPT_RT kernel (`6.8.1-1015-realtime`, `HZ=1000`), the
same 10-second period sweep passed with the default SCHED_OTHER application
and IRQ thread priority 50. Longer SCHED_OTHER tests at 16 frames were not
reliable: a 120-second run with four nice=10 CPU workers xran, as did the
following no-load run. With the application at SCHED_FIFO 40 and the IRQ
thread at 50, 16/48 with a two-period queue passed for 60 seconds unloaded
and 120 seconds with the same CPU load. Raising the IRQ thread to 90 and the
application to SCHED_FIFO 70 also passed those two durations. All four
successful long runs measured 3.458 ms with no correlation-window or signal
errors. A one-period playback queue still xran at IRQ 90/application 70.
These results show the tested setup needs real-time scheduling for longer
16-frame runs; they do not establish stability under arbitrary workloads.

The reported round trip estimates capture-to-application-to-playback-to-capture
delay. It uses the input block available when an output block was generated
and the correlated return sample. The estimate includes block scheduling and
the analog loopback, but no audio-processing workload. Raw correlation lag
alone is not round-trip latency because capture and playback start at different
offsets. At 48 kHz, a 48-frame ALSA buffer is 1 ms; it is **not** the measured
end-to-end delay. The hardware ring still holds three engine periods, and
playback queues two more periods in DMA.

## Sound servers (PipeWire, JACK)

Run these commands from `linux/snd-motu424/`.

`tools/audio_server_test.py` uses Python 3 and no extra Python modules. Run it
as the desktop user on the machine with the card. The test needs `gcc` and
ALSA headers for `tools/pcm_caps.c`; the JACK phases also need `jackd`,
`jack_lsp` and `pw-jack`. By default, it plays digital silence. With
`--loopback`, it sends a -40 dBFS tone to one output (24I/O output 19 by
default), which must be connected to the matching input. **Mute monitors.**
The test restores the PipeWire profile and forced quantum/rate afterwards.
During the JACK phase it switches off the card's PipeWire profile while
`jackd` runs.

```sh
python3 tools/audio_server_test.py --log /tmp/motu424-audio-servers
python3 tools/audio_server_test.py --loopback --out 18 --in 18
```

Use `--phases env,alsa,pipewire,pwjack,jack` to select phases:

* `alsa`: the constraint space per rate (channels, periods, period sizes,
  buffer), the `BATCH` flag, and the hardware pointer's step.
* `pipewire`: pro-audio nodes and formats; silent capture and playback at
  forced quanta 64–2048 and rates 44.1, 96 and 48 kHz, each checked for node
  errors, journal errors, xruns (pw-top `ERR`), hardware pointer speed
  (±1 % of the rate) and recorded length; the optional loopback; the
  stereo desktop profile.
* `pwjack`: JACK clients through PipeWire.
* `jack`: jackd with the ALSA backend at periods 24 (must be refused) to
  1024, `-n 2` and `-n 3`, 2 and 64 channels, 96 kHz and back; each run checks
  ports, xruns and a clean shutdown (a hang is a failure).

On the four-box setup with PipeWire 1.0.5, WirePlumber 0.4.17, JACK2 1.9.21
and Linux 6.8 PREEMPT_RT, all 34 checks passed. The loopback had no failures
across 1,796 five-millisecond blocks. Startup at 96 kHz takes 1.6 seconds,
including the AudioWire settle wait after a multiplier change.

With the earlier driver configuration—exact channel counts, three periods
only, period sizes of 16, 64, 256 or 1024 frames, and no `BATCH` flag—17
checks failed. PipeWire nodes would not start ("Channels doesn't match,
requested 64, got 96"), jackd `-n 2` hung in an xrun/restart loop, and the
driver refused periods of 32/48/128/512 frames and channel subsets.

Usage notes:

* PipeWire: select the "Pro Audio" profile for all channels (first 64 on
  PipeWire < 1.6), or the stereo profile for the first two channels as a
  desktop device. The pro-audio sink and source are separate drivers unless
  grouped; add a WirePlumber rule with a common `node.group` if both should
  share one graph clock.
* JACK: e.g. `jackd -d alsa -d hw:M424 -r 48000 -p 128 -n 2`. Stop or switch
  off the card's PipeWire profile first, or use `pw-jack`.

## PCM settle regression test

Run this command from `linux/snd-motu424/`.

This hardware test checks that a short, separately opened capture waits for
playback to settle after a 48-to-96 kHz switch. Playback is silent and capture
is discarded. The card must be idle at 48 kHz; the test restores that rate.
Pass the card ID and full capture/playback channel counts for your setup:

```sh
# CARD BASE_CAPTURE HIGH_PLAYBACK HIGH_CAPTURE
sh tools/pcm_settle_test.sh M424 96 24 24
```

## Userspace tests

From `linux/snd-motu424/`:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -I tools/shim -I . \
    tools/mixer_test.c motu424_mixer.c motu424_qmix.c -o /tmp/motu424-mixer-test
/tmp/motu424-mixer-test 48 -q -x

cc -std=gnu11 -Wall -Wextra -Werror -I tools/shim -I . \
    tools/iface_test.c motu424_iface.c -o /tmp/motu424-iface-test
/tmp/motu424-iface-test

cc -std=gnu11 -Wall -Wextra -Werror -I tools/shim -I . \
    tools/rawmidi_test.c motu424_rawmidi.c -o /tmp/motu424-rawmidi-test
/tmp/motu424-rawmidi-test

cc -std=gnu11 -Wall -Wextra -Werror -I tools/shim -I . \
    tools/control_test.c motu424_control.c motu424_iface.c -o /tmp/motu424-control-test
/tmp/motu424-control-test

cc -std=gnu11 -Wall -Wextra -Werror -I tools/shim -I . \
    tools/smpte_test.c motu424_smpte.c motu424_iface.c -o /tmp/motu424-smpte-test
/tmp/motu424-smpte-test
```

The mixer test covers DSP program generation, DMA and PIO downloads, fader
gains, input-table writes, trim updates, route allocation, mute/solo, stereo
transforms, talkback/listenback and dimming, meter-bank exchange, hotplug
reconciliation, capacity and timeout handling. The control test uses a
simulated ALSA core and backend to check registration, physical indices,
stereo mono-control notifications, meter priming, 2408/1296/HD192 control
validation and state-byte updates, deferred topology snapshots, error
propagation and clock-source selection.

The interface test runs the port state machine against simulated 24I/O, 2408,
2408mk3, 1296, HD192 and two self-describing boxes. It checks message pacing
and content, retries after a bad checksum, release messages, timeout handling,
2408 bank and 1296/HD192 update handshakes, and channel layouts—including a
box disappearing while a stream is open.

The SMPTE test uses a simulated DSP control block. It converts every frame in
a day across all formats, including drop-frame, and checks frame parameters
against MOTU's formulas at 44.1 and 48 kHz. It also covers source and
destination validation, address-source modes, generator registers at 1x and
4x, format following, 2408mk3 video/LTC/SMPTE-output bits, S/MUX masks and
retries during box reconfiguration.

The rawMIDI test covers duplex-port creation, open/close and trigger handling,
input delivery and output queue consumption.
