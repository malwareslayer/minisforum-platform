MODULE_NAME := minisforum_platform
obj-m := $(MODULE_NAME).o
$(MODULE_NAME)-y := src/minisforum_core.o src/minisforum_profile.o src/minisforum_hwmon.o

KVERSION ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVERSION)/build
PWD := $(shell pwd)

VERSION := 0.3.3
DKMS_DIR := /usr/src/$(MODULE_NAME)-$(VERSION)

.PHONY: all modules clean install load unload dkms-install dkms-uninstall deb

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f src/*.o src/.*.cmd

install: modules
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a $(KVERSION)

load:
	sudo insmod $(MODULE_NAME).ko

unload:
	sudo rmmod $(MODULE_NAME)

dkms-install: clean
	mkdir -p $(DKMS_DIR)
	cp -r Makefile dkms.conf src $(DKMS_DIR)/
	dkms add -m $(MODULE_NAME) -v $(VERSION)
	dkms build -m $(MODULE_NAME) -v $(VERSION)
	dkms install -m $(MODULE_NAME) -v $(VERSION)

dkms-uninstall:
	dkms remove -m $(MODULE_NAME) -v $(VERSION) --all || true
	rm -rf $(DKMS_DIR)

deb:
	./packaging/debian/build-deb.sh $(VERSION)

