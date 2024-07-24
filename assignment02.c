#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/uaccess.h>

#define DEBUG(...) printk(KERN_DEBUG __VA_ARGS__)
#define INFO(...)  printk(KERN_INFO __VA_ARGS__)
#define ALERT(...) printk(KERN_ALERT __VA_ARGS__)

#define D_NAME "assignment02"
#define C_NAME "assignment_class"

static int major = 0;

static int major_number;

module_param(major, int, S_IRUGO);
MODULE_PARM_DESC(major, "device major number");


static struct class *module_class = NULL;
static struct device *module_device = NULL;

// File operations
static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);

// Define file operations struct
static struct file_operations fops = {
    .open = device_open,
    .read = device_read,
    .write = device_write,
    .release = device_release,
};

static int device_open(struct inode *node, struct file *filep)
{
    INFO("%s: open the file\n", D_NAME);
    return 0;
} 
    
static int device_release(struct inode *node, struct file *filep)
{
    INFO("%s: close the file\n", D_NAME);
    return 0;
}

static int device_read(struct file * filep, char * buff, size_t size, loff_t * offset)
{
    INFO("%s: read %d bytes from the file\n", D_NAME, size);
    return 0;
}


static int device_write(struct file * filep, const char * buff, size_t size, loff_t * offset)
{
    INFO("%s: write %d bytes to the file\n", D_NAME, size);
    return size;
}

static int __init my_init(void)
{
    int ret;
    printk(KERN_INFO "Hello, module loaded at 0x%p\n", my_init);
    printk(KERN_INFO "The value of parameter major is %d\n", major);

    major_number = ret = register_chrdev(0, D_NAME, &fops);
    if (major_number < 0) 
    {
        ALERT("%s failed to register a major number\n", D_NAME);
        goto error;
    }
    INFO("%s: registered correctly with major number: %d\n", D_NAME, ret);

    module_class = class_create(C_NAME);
    if (IS_ERR(module_class)) 
    {
        ALERT("%s Failed to rregister device class: %s\n", D_NAME, C_NAME);
        ret = PTR_ERR(module_class);
        goto error_with_dev;
    }
    INFO("%s: device class registered correctlt\n", C_NAME);

    module_device = device_create(module_class, NULL, MKDEV(major_number, 0), NULL, D_NAME);
    if (IS_ERR(module_device))
    {
        ALERT("%s: Failed to create device\n");
        ret = PTR_ERR(module_device);
        goto error_with_class;
    }
    INFO("%s: create device correctly\n", D_NAME);

    return 0;

error_with_class:
    class_unregister(module_class);
    class_destroy(module_class);

error_with_dev:
    unregister_chrdev(major_number, D_NAME); 

error: 
    return ret;
}

static void __exit my_exit(void)
{
    INFO("Byte, module unloaded at 0x%p\n", my_exit);
    device_destroy(module_class, MKDEV(major_number, 0));
    class_unregister(module_class);
    class_destroy(module_class);
    unregister_chrdev(major_number, D_NAME);
}

module_init(my_init);
module_exit(my_exit);

MODULE_AUTHOR("Jiasheng Li");
MODULE_LICENSE("GPL");
