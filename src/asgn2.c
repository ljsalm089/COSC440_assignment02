# include <linux/types.h>   // for multiple kinds of types
# include <linux/cdev.h>
# include <linux/device.h>
# include <linux/fs.h>
# include <linux/module.h>
# include <linux/moduleparam.h>
# include <linux/init.h>
# include <linux/kdev_t.h>
# include <linux/slab.h> // For `kmalloc`
# include <linux/string.h>
# include <linux/uaccess.h> // `copy_from_user` `copy_to_user`
# include <linux/semaphore.h> // for struct `semaphore` and corresponding function 
# include <linux/wait.h>   // for `wait_queue_head_t`
# include <linux/sched.h> // for macro `current` to get current process info
# include <linux/spinlock.h> // for spinlock_t and related functions

# include "common.h"
# include "circular_buffer.h"
# include "page_buffer.h"

#define D_NAME "asgn2"
#define TAG "asgn2"
#define C_NAME "assignment_class"

#define PROC_CACHE_SIZE 100
#define SEQ_TYPE_MAXIMUM 0x01
#define SEQ_TYPE_TIPS 0x02
#define SEQ_TYPE_PAGE 0x03

static int major = 0;
module_param(major, int, S_IRUGO);

MODULE_PARM_DESC(major, "device major number");
MODULE_AUTHOR("Jiasheng Li");
MODULE_LICENSE("GPL");

// define  structure to save the data about the device
typedef struct {
    struct class *clazz;
    struct device *device;
    struct cdev dev;

    // the created entry in /proce for device
    // used to display some inner info like allocated pages addresses, value of variable max_process_count, etc.
    struct proc_dir_entry * proc_entry;

    // semaphore for reading, writing and memory maping
    struct semaphore sema;

    // wait queue for those processes open the file as read-only or read-write
    wait_queue_head_t wait_queue;

    // spin lock to protect count and write_only_count
    spinlock_t lock;

    // current using process id
    pid_t current_pid;
} DevData;
typedef DevData * PDevData;

// declare data for module
static PDevData d_data;

// function to change the access permissions of the deivce file
static char *asgn2_class_devnode(const struct device *dev, umode_t *mode)
{
    if (mode)
        *mode = 0444;
    return NULL;
}

static int device_open(struct inode *node, struct file *filep)
{
    D(TAG, "process(%d) try to open the device", currentpid);
    pid_t pid = currentpid;

    do {
        int should_wait = 1;
        spin_lock(&d_data->lock);
        // only when the current running process pid is -1, 
        // the process is permitted to keep going
        if (d_data->current_pid < 0) {
            d_data->current_pid = pid;
            should_wait = 0;
        }
        spin_unlock(&d_data->lock);

        if (should_wait) {
            D(TAG, "Process(%d) hasn't been granted the resource, keep waiting", pid);
            wait_event_interruptible_exclusive(d_data->wait_queue, 
                    d_data->current_pid) == -1;
            D(TAG, "Process(%d) is awake, check what happen", pid);
            if (signal_pending(current)) {
                D(TAG, "Process(%d) had received an signal, abort the open process", pid);
                return -ERESTARTSYS;
            }
        } else {
            break;
        }
    } while (true);
    D(D_NAME, "Process(%d) has gained the resource", pid);
    return SUCC;
} 
    
static int device_release(struct inode *node, struct file *filep)
{
    D(D_NAME, "Process(%d) close the device", currentpid);
    spin_lock(&d_data->lock);
    d_data->current_pid = -1;
    spin_unlock(&d_data->lock);
    
    // wake up one of the processes waiting for the resource
    wake_up_interruptible_nr(&d_data->wait_queue, 1);
    return 0;
}

static ssize_t device_read(struct file * filep, char * buff, size_t size, loff_t * offset)
{
    // in case the file is accessed from multiple processes/threads
    if (down_interruptible(&d_data->sema))
        return -ERESTARTSYS;

    PDevData p = d_data;
    size_t already_read_size = 0;

    D(D_NAME, "Process(%d) start to read %d bytes from the file\n", currentpid, size);

    // TODO need to read data from page buffer

release:
    up(&d_data->sema);

    return already_read_size;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .read = device_read,
    .release = device_release,
};

static int allocate_major_number(dev_t *devno, int *major)
{
    int ret = -1;
    D(D_NAME, "Try to register for major number: %d", *major);
    *devno = MKDEV(*major, 0);
    if (*devno) 
        ret = register_chrdev_region(*devno, 1, D_NAME);

    if (ret < 0) {
        W(D_NAME, "Can't use this major number: %d", *major);
        ret = alloc_chrdev_region(devno, 0, 1, D_NAME);
        *major = MAJOR(*devno);
    }
    return ret;
}

static void release_major_number(dev_t devno)
{
    unregister_chrdev_region(devno, 1);
}

static int __init asgn2_init(void)
{
    int ret;
    I(D_NAME, "Hello, module loaded at 0x%p", asgn2_init);
    I(D_NAME, "The value of parameter major is %d", major);

    dev_t dev_no;

    // allocate major number for device
    ret = allocate_major_number(&dev_no, &major);
    if (ret < 0) return ret; 

    D(D_NAME, "registered correctly with major number: %d", major);

    // allocate memory to store data
    d_data = kmalloc(sizeof(DevData), GFP_KERNEL);
    if (!d_data) {
        ret = -ENOMEM;
        E(D_NAME, "failed to allocate memory to store data");
        goto error_with_major;
    }
    memset(d_data, 0, sizeof(DevData));
    d_data->current_pid = -1;
    init_waitqueue_head(&d_data->wait_queue);

    // initialise the semaphore
    sema_init(&d_data->sema, 1);

    // initialise dev
    cdev_init(&d_data->dev, &fops);
    d_data->dev.owner = THIS_MODULE;

    ret = cdev_add(&d_data->dev, dev_no, 1);
    if (ret < 0) {
        E(D_NAME, "failed to add cdev");
        goto error_with_data;
    }

    D(D_NAME, "add cdev successfully");

    // create class
    d_data->clazz = class_create(C_NAME);
    if (IS_ERR(d_data->clazz)) {
        ret = PTR_ERR(d_data->clazz);
        E(D_NAME, "failed to create class(%s) for device: %d", C_NAME, ret);
        goto error_with_cdev;
    }
    d_data->clazz->devnode = asgn2_class_devnode;
    D(D_NAME, "create class(%s) successfully", C_NAME);

    // create device
    d_data->device = device_create(d_data->clazz, NULL, dev_no, NULL, D_NAME);
    if (IS_ERR(d_data->device)) {
        ret = PTR_ERR(d_data->device);
        E(D_NAME, "failed to create device: %d", ret);
        goto error_with_class;
    }
    D(D_NAME, "create device successfully");
    I(D_NAME, "initialise successfully");

    return 0;
error_with_class:
    class_destroy(d_data->clazz);

error_with_cdev:
    cdev_del(&d_data->dev);

error_with_data:
    kfree(d_data);

error_with_major:
    release_major_number(dev_no);
    return ret;
}

static void __exit asgn2_exit(void)
{
    I(D_NAME, "Byte, module unloaded at 0x%p\n", asgn2_exit);

    dev_t dev_no = MKDEV(major, 0);    
    device_destroy(d_data->clazz, dev_no);
    class_destroy(d_data->clazz);
    cdev_del(&d_data->dev);
    kfree(d_data);
    release_major_number(dev_no);
}

module_init(asgn2_init);
module_exit(asgn2_exit);
