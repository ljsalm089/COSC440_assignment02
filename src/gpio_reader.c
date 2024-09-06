# include <linux/types.h>
# include <linux/gpio.h>
# include <linux/version.h>
# include <linux/delay.h>
# include <linux/slab.h> // For `kmalloc`
# include <linux/string.h>
# include <linux/platform_device.h>

# if LINUX_VERSION_CODE > KERNEL_VERSION(3, 3, 0)
    # include <asm/switch_to.h>
# else
    # include <asm/system.h>
# endif

# include <common.h>
# include <gpio_reader.h>


#define TAG "GPIOReader"
#define BCM2837_PERI_BASE 0x3f000000

#define CONVERT(r, inner) PGReader r = _convert_preader((inner));


typedef struct {
    GPIOReader inner;

    u32 gpio_base;
} _GReader;

struct gpio gpio_pins[] = {
    { 7, GPIOF_IN, "GPIO7" },
    { 8, GPIOF_OUT_INIT_HIGH, "GPIO8" },
    { 17, GPIOF_IN, "GPIO17" },
    { 18, GPIOF_OUT_INIT_HIGH, "GPIO18" },
    { 22, GPIOF_IN, "GPIO22" },
    { 23, GPIOF_OUT_INIT_HIGH, "GPIO23" },
    { 24, GPIOF_IN, "GPIO24" },
    { 25, GPIOF_OUT_INIT_HIGH, "GPIO25" },
    { 4, GPIOF_OUT_INIT_LOW, "GPIO4" },
    { 27, GPIOF_IN, "GPIO27" },

};

typedef _GReader * PGReader;

PGReader _convert_preader(PGPIOReader r)
{
    PGReader reader = (PGReader) ((char *) r - offsetof(_GReader, inner));
    return reader;
}

static inline u32 _gpio_inw(u32 addr)
{
    u32 data;

    asm volatile("ldr %0,[%1]" : "=r"(data) : "r"(addr));
    return data;
}

static inline void _gpio_outw(u32 addr, u32 data)
{
    asm volatile("str %1,[%0]" : : "r"(addr), "r"(data));
}

u8 _read_half_byte(u32 base)
{
    u32 c;
    u8 r;

    r = 0;
    c = _gpio_inw(base + 0x34);
    if (c & (1 << 7)) r |= 1;
    if (c & (1 << 17)) r |= 2;
    if (c & (1 << 22)) r |= 4;
    if (c & (1 << 24)) r |= 8;

    return r;
}

static void _write_to_gpio(u32 base, char c)
{
    volatile unsigned *gpio_set, *gpio_clear;

    gpio_set = (unsigned *)((char *)base + 0x1c);
    gpio_clear = (unsigned *)((char *)base + 0x28);

    if(c & 1) *gpio_set = 1 << 8;
    else *gpio_clear = 1 << 8;
    udelay(1);

    if(c & 2) *gpio_set = 1 << 18;
    else *gpio_clear = 1 << 18;
    udelay(1);

    if(c & 4) *gpio_set = 1 << 23;
    else *gpio_clear = 1 << 23;
    udelay(1);

    if(c & 8) *gpio_set = 1 << 25;
    else *gpio_clear = 1 << 25;
    udelay(1);
}


PGPIOReader create_new_gpio_reader(irqreturn_t (* handler)(int, void *)) 
{
    int ret;
    PGReader reader = (PGReader) kmalloc(sizeof(_GReader), GFP_KERNEL);
    if (IS_ERR(reader)) {
        ret = PTR_ERR(reader);
        E(TAG, "Unable to allocate memory for PGReader: %d", ret);
        return NULL;
    }
    memset(reader, 0, sizeof(_GReader));

    reader->gpio_base = (u32) ioremap(BCM2837_PERI_BASE + 0x200000, 4096);
    D(TAG, "Map gpio base to %x", reader->gpio_base);

    ret = gpio_request_array(gpio_pins, ARRAY_SIZE(gpio_pins));
    if (ret) {
        E(TAG, "Unable to request GPIOs for the device: %d", ret);
        goto e_with_reader;
    }

    struct gpio pin = gpio_pins[ARRAY_SIZE(gpio_pins) - 1];
    ret = gpio_to_irq(pin.gpio);
    if (ret < 0) {
        E(TAG, "Unable to request IRQ for gpio %d: %d", pin.gpio, ret);
        goto e_with_array;
    }
    D(TAG, "Successfully requested IRQ# %d for %s", ret, pin.label);
    reader->inner.irq_num = ret;

    ret = request_irq(ret, handler, IRQF_TRIGGER_RISING | IRQF_ONESHOT, "gpio27", NULL);
    if (ret) {
        E(TAG, "Unable to request IRQ for device: %d", ret);
        goto e_with_array;
    }

    // light up all the led on the dummy device
    _write_to_gpio(reader->gpio_base, 15);
    return &reader->inner;

e_with_array:
    gpio_free_array(gpio_pins, ARRAY_SIZE(gpio_pins));
    iounmap((void *)reader->gpio_base);

e_with_reader:
    kfree(reader);

    return NULL;
}

void release_gpio_reader(PGPIOReader reader)
{
    CONVERT(r, reader);
    gpio_free_array(gpio_pins, ARRAY_SIZE(gpio_pins));
    iounmap((void *) r->gpio_base);
    kfree(r);
}

char read_half_byte_from_reader(PGPIOReader reader)
{
    CONVERT(r, reader);
    return _read_half_byte(r->gpio_base);
}
