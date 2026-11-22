# nitro-wmi — port de NitroSense a Linux
# El compilador se detecta según el kernel destino:
#   CONFIG_CC_IS_CLANG=y  → CC=clang LD=ld.lld (kernels CachyOS mainline)
#   si no                 → GCC (p. ej. cachyos-lts)
KVER ?= $(shell uname -r)
KBUILD ?= /lib/modules/$(KVER)/build
CLANG_K := $(shell grep -c "CONFIG_CC_IS_CLANG=y" $(KBUILD)/.config 2>/dev/null)
ifeq ($(strip $(CLANG_K)),1)
  KOPT := CC=clang LD=ld.lld
else
  KOPT :=
endif
KMAKE ?= $(MAKE) -C $(KBUILD) M=$(CURDIR) $(KOPT)

obj-m += nitro_wmi.o

.PHONY: all module cli daemon client load unload clean

all: module cli daemon client

cli: nitro.c
	$(CC) -O2 -Wall -Wextra -o nitro nitro.c

daemon: nitro_daemon.c
	$(CC) -O2 -Wall -Wextra -o nitro-daemon nitro_daemon.c

client: nsclient.c
	$(CC) -O2 -Wall -Wextra -o nsclient nsclient.c

module:
	$(KMAKE) modules

load: module
	-@sudo rmmod nitro_wmi 2>/dev/null
	sudo insmod ./nitro_wmi.ko

unload:
	sudo rmmod nitro_wmi

clean:
	$(KMAKE) clean
	rm -f nitro
