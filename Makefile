MODULE_NAME = asgn1

obj-m := $(MODULE_NAME).o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:module


module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
