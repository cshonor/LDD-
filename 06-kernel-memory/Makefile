obj-m += memdemo.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all clean

all: modules
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	$(CC) $(wildcard test_*.c) -o test_twodev 2>/dev/null || true

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_twodev
