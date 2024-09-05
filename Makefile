
obj-m := asgn2.o

asgn2-y := src/asgn2.o
asgn2-y += src/circular_buffer.o

ccflags-y := -I$(src)/include

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
