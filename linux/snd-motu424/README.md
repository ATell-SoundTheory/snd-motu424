# snd-motu424

Experimental ALSA driver for the MOTU PCI-424, PCI-424X, and PCIe-424 family.

- [Driver user guide](../../docs/USER_GUIDE.md) — hardware status, firmware,
  installation, configuration, controls, and limitations.
- [Technical documentation](../../docs/README.md) — reverse-engineered
  hardware, AudioWire, DSP mixer, and SMPTE references.
- [Q-Mix](qmix-ui/README.md) — optional browser mixer.

Build the external module from this directory with the headers for the running
Linux kernel installed:

```sh
make
```

Vendor firmware is required at runtime and is not included in this repository.
