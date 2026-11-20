# SPDX-License-Identifier: GPL-2.0-only
obj-m += snd-motu424.o
snd-motu424-y := motu424_core.o motu424_dsp.o motu424_pcm.o motu424_mixer.o \
		  motu424_iface.o motu424_qmix.o motu424_control.o \
		  motu424_rawmidi.o motu424_smpte.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a
