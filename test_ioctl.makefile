test:
	$(CC) test_ioctl.c user_common.h -o test_ioctl


all: test

clean:
		rm test_ioctl
