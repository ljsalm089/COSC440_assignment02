#ifndef __GPIO_READER_H__
#define __GPIO_READER_H__

# include <linux/interrupt.h>
# include <linux/irq.h>
# include <linux/irqdesc.h>

typedef struct {
    int req_number;
} GPIOReader;

typedef GPIOReader * PGPIOReader;

PGPIOReader creater_new_gpio_reader(irqreturn_t (* handler)(int, void *));

void release_gpio_reader(PGPIOReader reader);

char read_half_byte(PGPIOReader reader);

#endif  // __GPIO_READER_H__
