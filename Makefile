# SPDX-License-Identifier: GPL-2.0-or-later

obj-m += drivers/firewire/
obj-m += drivers/media/firewire/
obj-m += drivers/target/sbp/
obj-m += sound/firewire/

ifndef KERNELRELEASE
KERNELRELEASE := $(shell uname -r)
endif

KDIR := /lib/modules/$(KERNELRELEASE)/build
PWD := $(shell pwd)

all:
	$(MAKE) W=1 -C $(KDIR) M=$(PWD) modules

check: clean
	$(MAKE) W=1 C=1 CF=-D__CHECK_ENDIAN__ -C $(KDIR) M=$(PWD) modules

install:
	$(MAKE) W=1 -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) W=1 -C $(KDIR) M=$(PWD) clean
