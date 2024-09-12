obj-m := asgn.o # gpio.o

asgn-y := src/asgn2.o 
asgn-y += src/circular_buffer.o src/page_buffer.o src/gpio_reader.o src/mem_cache.o


ccflags-y := -I$(src)/include

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
