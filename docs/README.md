# Documentation

These documents describe **snd-motu424**, an experimental Linux ALSA driver
for the MOTU PCI-424 family, and its optional Q-Mix user-space application.
The project aims to enable multichannel audio and device control on Linux;
its hardware protocols were recovered through reverse engineering, and some
paths remain unverified. For the project overview, start at the
[repository README](../README.md).

Choose the guide for your task:

- **Use the driver:** [User guide](USER_GUIDE.md) covers supported hardware,
  installation, firmware extraction, controls, configuration, and limitations.
- **Operate Q-Mix:** [Q-Mix overview](QMIX.md) describes the interface;
  [Q-Mix README](../linux/snd-motu424/qmix-ui/README.md) covers installation
  and trusted-network access.
- **Understand the implementation:** the documents below record the
  reverse-engineered hardware and DSP behavior.

## Technical reference

- [HARDWARE.md](HARDWARE.md) — card registers, firmware loading, boot, DMA,
  interrupts, and memory layout.
- [INTERFACES.md](INTERFACES.md) — AudioWire discovery, interface layouts,
  clocking, and control protocols.
- [MIXER.md](MIXER.md) — DSP mixer generation, instruction scheduling, and
  verification.
- [SMPTE.md](SMPTE.md) — LTC, video sync, and generator controls.
- [Reverse-engineering statement](REVERSE_ENGINEERING.md) — documented
  research sources, repository scope, and provenance limitations.

The technical references distinguish hardware-tested behavior from static
analysis and uncertain inferences. They are not required reading for ordinary
driver installation or use.
