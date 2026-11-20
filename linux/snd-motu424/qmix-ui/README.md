# Q-Mix

This optional user-space application provides a browser UI for the Q-Mix ALSA
controls exposed by `snd-motu424`. It does not add HTTP or application code to
the kernel driver.

The interface covers physical input trims and switches, per-bus route level,
pan, mute and solo, bus level and mute, stereo balance/width/pan, talkback,
listenback, and raw DSP peak meters. It also stores and recalls profiles in the
running user's data directory. The Channels page stores custom names for all
96 physical inputs and 48 stereo output pairs on the mixer host; blank names
fall back to the hardware identifiers. Selecting a profile only previews it;
applying one requires explicit confirmation. During apply, output buses are
disabled while changed controls are written and restored last. If any write
fails, the service reports the partial update and leaves buses disabled where
it could.
The red **Mute** buttons indicate a muted bus or input-to-bus route; **Solo**
buttons light yellow when engaged.

The [Q-Mix surface overview](../../../docs/QMIX.md) includes screenshots and
describes the Mix, Devices, Channels, and Profiles pages.

The Mix page uses a console layout with the 96-input meter bridge above
horizontal-scrolling input strips, the output selector on the left, and the
selected bus master on the right. Hardware labels include the physical port
and source type where the driver can identify them. Input and output indices
remain fixed physical hardware indices; unavailable hardware is marked but its
stored controls remain editable. The power button in each strip header is the
global Q-Mix input gate: the driver sets that source's DSP trim factor to zero
when it is off. Route `Mute` suppresses only that input's route to the selected
bus. DSP Trim is shown in dB relative to the driver's unity value of 64 (for
example, 128 is approximately +6 dB). `Hide` removes a strip from the Mix page
for all browsers; `Show hidden` in the strip header brings hidden strips back
so they can be unhidden. The meter bridge always shows all 96 inputs, grouped
by their AudioWire device in fixed 24-channel port ranges.
Long names are displayed beside their meter so they do not consume the height
needed by every channel strip.
The meter bridge can be collapsed to its heading to give the strips more room;
its peak holds and clip indicators continue updating while it is collapsed.
On short screens the Mix page scrolls vertically to keep the strips usable.
`Show connected only` is a display filter for inputs reported present by the driver;
it never connects, disconnects or changes an audio route.

Stereo link is stored on the mixer host. A linked pair is one strip whose
gain, balance, width and pan are another description of the two channels' route
levels and pans, so either view can be edited and the other follows:

- gain is the louder of the two route levels;
- balance lowers the other side by `1 - offset²`, where `offset` runs from 0 at
  the centre to 1 at either end (the law of the driver's `Stereo Balance`);
- pan is the midpoint of the two mono pans;
- width is `64 + (rightPan - leftPan) / 2`: left is reversed, centre is mono,
  right is normal stereo. Half-step coordinates preserve odd pan differences.

Linking leaves both route levels and both pans unchanged, and gives the right channel the left
channel's trim, input switch, mute and solo on every bus. While linked, the
service keeps the shared trim and switches synchronized. Direct edits of either
mono gain or pan are reflected in the stereo representation without changing
the other channel's gain or pan. Moving stereo pan translates both pans equally
until either reaches an edge; width changes separation while preserving the
midpoint. The controls' available ranges reflect these boundaries, rather than
clipping a channel and losing width or pan information. All 129-by-129 mono pan
combinations convert to stereo and back exactly. Shared settings are synchronized
when the service starts, after a profile is applied, and whenever its bus is
shown. Unlinking changes nothing in the
mixer. The balance set on a pair is remembered while the service runs, so
pulling the stereo fader to `−∞` and back restores it; after a restart the
balance is recomputed from the two levels. The driver's own `Stereo Balance`
and `Stereo Width` controls are not used by the Mix page, because setting one
resets the other.

The hardware has no phantom power, preamp gain or pad controls. The Devices page
lists the live ALSA hardware settings, with the PCI(e) card first and then one
section per recognized AudioWire device in port order, grouped by function.
The external-clock indicator shows detection as unavailable: the driver does
not report external clock lock or rate. Only active published
settings are shown; a recognized device without settings still gets a section.
This includes clocking, bank formats, reference levels and supported AES, meter
hold, S/MUX and SMPTE options. Read-only settings are displayed without editors.
Unknown physical units/mode meanings remain explicitly labelled as driver codes.
Settings are refreshed while the page is open and writes use ALSA readback.
Clock/bank changes can fail while PCM is open; interface changes are asynchronous
and controls are unavailable until the device is configured again. Readback is
the driver's requested state, not hardware confirmation. These settings are not
stored in mixer profiles and retain the driver's existing reset/persistence
behavior. Changing reference levels can change input gain; generating SMPTE can
replace audio on its destination. See the [driver user guide](../../../docs/USER_GUIDE.md) for restrictions
and hardware-test status.

Stereo strips have a yellow-green tint and matching highlights; master faders are blue.
Meter bars rise immediately and fall at 20 dB per 1.7 seconds, following the
return time described for digital peak meters in IEC TR 60268-18. This is a
display characteristic, not a claim of full IEC meter compliance. Peak
readouts have a separate five-second hold before gradual decay; click a lit
clip indicator to clear it. A stereo strip and each bus-master pair share one
clip indicator. Changing the selected output resets the bus-master peak holds
and clip indicators and waits for meter data for that output.

## Requirements

- Linux with the `snd-motu424` driver loaded and the `M424` ALSA control device
  available to the user running the service.
- Python 3.10 or later and the `libasound` runtime library.
- Node.js and npm to build the static frontend and run the complete test suite.
- A private LAN/VPN IP address and a trusted TLS certificate for access from
  other devices. The service refuses wildcard or public bind addresses.

The service should run as an ordinary user with ALSA device permissions, not as
root. Profile files are stored under
`~/.local/share/motu424-qmix/profiles/` with user-only directory and file
permissions. Channel names are stored in
`~/.local/share/motu424-qmix/channel-names.json`, shared across browser
clients, and are not part of mixer profiles. Stereo-pair mode is stored in
`~/.local/share/motu424-qmix/stereo-pairs.json` and hidden strips in
`~/.local/share/motu424-qmix/hidden-inputs.json`.

When upgrading an earlier development installation, move its existing data
and TLS files into the corresponding `motu424-qmix` directories before starting
the renamed service. The renamed ALSA controls require the matching driver.

## Build and test

From this directory:

```sh
npm ci
npm run build
npm test
```

The service uses Python's standard library and calls ALSA's control API through
`libasound`; it does not invoke `amixer`. The tests use a fake ALSA control
device. They do not replace testing with a PCI-424 device.

## Run on the mixer host

Build the frontend once, then start the service as the logged-in user in
normal pairing mode:

```sh
PYTHONPATH=. python3 -m qmixui.server --card M424
```

Open `http://127.0.0.1:8765/` on the mixer host. On startup, the service prints
a pairing code. Enter it in the browser's pairing form; pair each browser
profile or device separately. A paired session expires after 12 hours. A
service restart clears all sessions and generates a new pairing code.

### If Q-Mix runs as a systemd user service

Systemd captures the service's output in the user journal. For a unit named
`motu424-qmix-web.service`, retrieve the latest pairing code with:

```sh
journalctl --user -u motu424-qmix-web.service -b -o cat --no-pager \
  | grep -F 'Pairing code: ' | tail -n 1
```

Replace the unit name if yours differs. The journal can contain codes from
earlier service starts; use only the latest code for the current service
process, and do not share it in logs, screenshots or public support requests.
If no pairing-code line appears, check whether the service was started with
`--dev-no-auth`.

For development, add `--dev-no-auth` to skip pairing and code entry. This
disables authentication entirely: any client that can reach an allowed service
host can control the mixer. The service displays a warning in the terminal and
UI; TLS, Host validation, and Origin/CSRF checks remain enabled but do not
replace authentication. Use this only on a trusted development network, and
remove the flag for normal use.

## Enable trusted LAN/VPN devices

Use a private address assigned to an interface reachable by the intended
trusted LAN/VPN clients. Provide a TLS certificate whose subject alternative
name matches the IP address and/or hostname used by browsers. A certificate
from a trusted local CA is preferred.
For a self-signed certificate, install and trust it on each client device; a
browser certificate warning does not provide equivalent protection.

For example, replace the sample address and hostname with the mixer's actual
LAN address and local DNS name:

```sh
mkdir -p ~/.config/motu424-qmix/tls
openssl req -x509 -newkey rsa:3072 -sha256 -days 365 -nodes \
  -keyout ~/.config/motu424-qmix/tls/server.key \
  -out ~/.config/motu424-qmix/tls/server.crt \
  -subj "/CN=motu424.local" \
  -addext "subjectAltName=IP:192.168.1.20,DNS:motu424.local"
chmod 600 ~/.config/motu424-qmix/tls/server.key

PYTHONPATH=. python3 -m qmixui.server \
  --card M424 \
  --listen 192.168.1.20 \
  --port 8765 \
  --allowed-host motu424.local \
  --tls-cert ~/.config/motu424-qmix/tls/server.crt \
  --tls-key ~/.config/motu424-qmix/tls/server.key
```

This example uses normal pairing. The `--dev-no-auth` option disables
authentication and is strictly for development; do not use it on a shared
LAN or VPN.

Allow inbound TCP port 8765 only from trusted LAN/VPN clients in the host
firewall. Do not forward the port from a router or expose the service to the
public internet. Access through a VPN is supported when it routes clients into
the trusted private network; public or arbitrary Internet access is not
supported.

The service requires TLS for non-loopback binds, accepts only the configured
host names, rate-limits pairing attempts, and normally requires a pairing code
plus per-session CSRF tokens for control writes. It does not enable CORS.

## Frontend development

On the Linux host, run the companion service on loopback with
`--dev-origin`, then run Vite in another terminal:

```sh
PYTHONPATH=. python3 -m qmixui.server --card M424 --dev-origin
npm run dev
```

Open `http://127.0.0.1:5173/`. The Vite server proxies API requests to the
loopback companion service. `--dev-origin` is rejected for LAN binds.

## Operational notes

- Route and bus controls affect input monitoring, not host PCM playback.
- The strip power button is the driver's `Q-Mix Input Switch`; it gates the
  input before all bus routes by setting its DSP trim factor to zero. Route
  `Mute` disables only that input-to-bus route; it does not mute the same input
  on other buses.
- Hardware monitoring while PCM is idle has not been verified. Keep external
  monitors muted during initial setup and route testing.
- Meter values are peaks in dBFS. The DSP reports `|24-bit sample| >> 8`, so
  the Mix page shows `20 * log10(raw_peak / 32768)`; a full-scale sample reads
  32767 and lights the clip indicator. This comes from the DSP firmware and
  was confirmed against a simultaneous ALSA capture (see "Peak value format"
  in `docs/MIXER.md`). The DSP measures inputs before the trim stage and the
  input switch, so input meters multiply the peak by `trim / 64` and read
  `−∞` while the input is switched off: they show the level that is recorded
  and monitored. The bus-master values are not trim-corrected. According to
  the firmware, the first pair (labelled `HOST`) is the monitor mix without
  host playback and the second (`MIX`) the complete output; this has not been
  checked with playback.
- The bridge and channel meters use green through -12 dBFS,
  yellow through -3 dBFS, red above -3 dBFS, a 5-second peak hold, and a
  time-based falloff bounded at -60 dBFS. The clip latch is clickable. One
  server-side reader streams meters at a target 60 Hz while a
  Mix-page stream is connected. Input banks alternate so all 96 strips receive
  samples (about 30 Hz per bank when the target rate is sustained). Multiple
  browsers share that one reader. Do not read `Q-Mix DSP Peak` from another
  application while the stream is active: each ALSA read swaps and consumes
  the DSP peak buffers. Selecting an output meter may consume DSP resources
  and can be rejected by the driver.
- Fader readouts and scale marks use the digital-volume mapping from MOTU's macOS mixer,
  `40 * log10(value / 32768)`; a zero value is shown as `−∞ dB`. The Mac app
  exposes controls for other hardware, but this PCI-424 UI does not show
  unsupported Pad, Phantom or preamp Gain controls.
- The talkback and listenback dim dials show nominal attenuation as
  `40 * log10(dim / 128)`, with zero shown as `−∞ dB`. The driver multiplies
  each route's fader value by `dim / 128` before applying the squared fader
  gain, so integer rounding and DSP gain quantization can affect the actual
  attenuation, especially at low levels.
- Automatic bank alternation is covered by the fake-DSP harness but has not
  yet been verified on hardware with known signals on both input banks.
- ALSA write errors and uncertain readback are shown in the UI. On a partial
  profile apply, inspect the live state before enabling output buses.
- This interface does not add unsupported driver features or claim that
  experimental Q-Mix paths are hardware-verified.
