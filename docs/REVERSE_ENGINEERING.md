# Reverse-engineering provenance

This is a factual description of the research sources recorded in this
repository, not legal advice or a conclusion about the legality of any
particular act or distribution.

The technical documentation states that the PCI-424 host interface and
AudioWire behavior were recovered by static analysis of MOTU's macOS
`MOTUPCIAudio.kext` and cross-checked against the Windows `MOTUAW.sys` driver.
The DSP mixer documentation describes analysis of the macOS driver and
bit-for-bit comparisons of the Linux mixer-program generator against an
emulated reference implementation. Hardware-test results and behavior inferred
from binary analysis are identified separately in the technical documents.

The tracked source tree contains the Linux driver, Q-Mix source, test tools,
and documentation. It does not distribute vendor installer packages or
extracted firmware images. The extraction tool operates on an installer
supplied by the user; any vendor firmware remains separate from this
project's license.

No separate analyst/implementer team or other formal clean-room procedure is
documented in this repository. Accordingly, the project makes no formal
clean-room claim. This statement does not determine whether reverse engineering
or distribution is permitted in any jurisdiction, and it does not replace
advice from qualified counsel.

MOTU product and software names are used to identify compatible hardware and
documented research sources. Nothing in this repository implies MOTU
affiliation or endorsement.
