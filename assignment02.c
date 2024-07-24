#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

static int major = 0;

module_param(major, int, S_IRUGO);
MODULE_PARM_DESC(major, "device major number");


static int __init my_init(void)
{
    printk(KERN_INFO "Hello, module loaded at 0x%p\n", my_init);
    printk(KERN_INFO "The value of parameter major is %d\n", major);
    return 0;
}

static void __exit my_exit(void)
{
    printk(KERN_INFO "Byte, module unloaded at 0x%p\n", my_exit);
}

module_init(my_init);
module_exit(my_exit);

MODULE_AUTHOR("Jiasheng Li");
MODULE_LICENSE("GPL");
