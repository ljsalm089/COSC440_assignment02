/**
 * Author: Jiasheng Li
 * Date: 11 Sept. 2024
 * Description: Linux module for assignment 2 of cosc440. Implement an character device which acts like a pipe file, 
 *              reads half byte of data from gpio each time, assembles them and return to user program.
 */
# include <linux/types.h>   // for multiple kinds of types
# include <linux/cdev.h>
# include <linux/device.h>
# include <linux/fs.h>
# include <linux/module.h>
# include <linux/kernel.h>
# include <linux/moduleparam.h>
# include <linux/init.h>
# include <linux/kdev_t.h>
# include <linux/string.h>
# include <linux/uaccess.h> // `copy_from_user` `copy_to_user`
# include <linux/semaphore.h> // for struct `semaphore` and corresponding function 
# include <linux/wait.h>   // for `wait_queue_head_t`
# include <linux/sched.h> // for macro `current` to get current process info
# include <linux/spinlock.h> // for spinlock_t and related functions

# include "common.h"
# include "circular_buffer.h"
# include "delimiter_buffer.h"
# include "gpio_reader.h"
# include "mem_cache.h"

#define D_NAME "asgn2"
#define TAG "asgn2"
#define C_NAME "assignment_class"

#define C_BUFFER_SIZE 30

static const char DELIMITER = '\0';

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

    // semaphore for reading, writing and memory maping
    struct semaphore sema;

    // wait queue for those processes open the file as read-only or read-write
    wait_queue_head_t wait_queue;

    // wait queue for those processes try to read but there is no data in the buffer
    wait_queue_head_t read_queue;

    // spin lock to protect count and write_only_count
    spinlock_t lock;

    // current using process id
    pid_t current_pid;

    // page buffer which includs delimiter detecter
    PDBuffer p_buff;

    // if there are some process waiting to read
    atomic_t waiting_for_read;

    // circular buffer which interrupt handler read data into
    // and tasklet read data from
    PCBuffer c_buff;
    // used to synchronise between interrupt and tasklet
    spinlock_t cbuff_lock;

    // encapsulate the operations of gpio, used to read data from gpio
    PGPIOReader reader;
    // temporarily store the half byte data
    char half_byte;
    // indicated when to conbine two half byte into one byte
    int counter;

    // flag indicates if the tasklet has been started
    int tasklet_running;

    // the tasklet which migrate data from circular buffer to page buffer
    struct tasklet_struct cbuffer_tasklet;
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

static void migration_tasklet(unsigned long data) 
{
    D(TAG, "The tasklet has been triggered");
    int buff_size = 10;
    char * tmpbuffer = (char *) alloc_mem(buff_size * sizeof(char));
    if (NULL == tmpbuffer) {
        E(TAG, "Unable to allocate memory to migrate data from circular "
                "buffer to page buffer");
        return;
    } else {
        D(TAG, "Allocated memory successfully: %lu", P2L(tmpbuffer));
    }

    size_t read_size = 0;
    size_t total_size = 0;
    size_t write_size = 0;

    do {
        spin_lock_wrapper(&d_data->cbuff_lock);
        D(TAG, "read data from circular buffer in the tasklet");
        read_size = read_from_cbuffer(d_data->c_buff, tmpbuffer, buff_size);
        D(TAG, "read data from circular buffer in the tasklet, read size: %d", read_size);
        if (read_size < buff_size) {
            d_data->tasklet_running = 0;
        }
        spin_unlock_wrapper(&d_data->cbuff_lock);

        D(TAG, "write data into dbuffer, write size: %d", read_size);
        // write_size = read_size;
        write_size = write_into_dbuffer(d_data->p_buff, tmpbuffer, read_size);
        D(TAG, "Migrated %d bytes into the page buffer", read_size);
        total_size += write_size;
    } while (read_size == buff_size && read_size == write_size);
    D(TAG, "Migrated %d bytes into the page buffer", total_size);

    if (total_size > 0) {
        atomic_set(&d_data->waiting_for_read, 0);
    }

    if (total_size > 0) {
        wake_up_interruptible_nr(&d_data->read_queue, 1);
    }

    release_mem((void *) tmpbuffer);
}

static irqreturn_t read_trigger(int req, void *dev_id)
{
    D(TAG, "Trigger the interrupt handler");
    char r = read_half_byte_from_reader(d_data->reader);
    if (d_data->counter % 2 == 0) {
        d_data->half_byte = r;
    } else {
        r = (d_data->half_byte << 4 | r);

        spin_lock_wrapper(&d_data->cbuff_lock);

        write_into_cbuffer(d_data->c_buff, &r, 1);
        if (cbuffer_size(d_data->c_buff) > cbuffer_available_size(d_data->c_buff) 
                || r == DELIMITER) {
            D(TAG, "Need to check if tasklet is running");
            if (!d_data->tasklet_running) {
                D(TAG, "The tasklet is not running, trigger to migrate data in circular buffer to page buffer");
                d_data->tasklet_running = 1;
                tasklet_schedule(&d_data->cbuffer_tasklet);
            }
            if (r == DELIMITER) {
                D(TAG, "Read a delimiter into the circular bufffer");
            }
        }

        spin_unlock_wrapper(&d_data->cbuff_lock);
    }
    d_data->counter ++;
    D(TAG, "Already wrote %d bytes into the circular buffer", d_data->counter / 2);
    return 0;
}

static int device_open(struct inode *node, struct file *filep)
{
    D(TAG, "process(%d) try to open the device", currentpid);
    pid_t pid = currentpid;

    do {
        int should_wait = 1;
        spin_lock_wrapper(&d_data->lock);
        // only when the current running process pid is -1, 
        // the process is permitted to keep going
        if (d_data->current_pid < 0) {
            d_data->current_pid = pid;
            should_wait = 0;
        }
        spin_unlock_wrapper(&d_data->lock);

        if (should_wait) {
            D(TAG, "Process(%d) hasn't been granted the resource, keep waiting", pid);
            wait_event_interruptible_exclusive(d_data->wait_queue, 
                    d_data->current_pid == -1);
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
    dbuffer_end_phase_reading(d_data->p_buff);
    D(D_NAME, "Process(%d) close the device", currentpid);
    spin_lock_wrapper(&d_data->lock);
    d_data->current_pid = -1;
    spin_unlock_wrapper(&d_data->lock);
    
    // wake up one of the processes waiting for the resource
    wake_up_interruptible_nr(&d_data->wait_queue, 1);
    return 0;
}

static ssize_t device_read(struct file * filep, char __user * buff, 
        size_t size, loff_t * offset)
{
    D(TAG, "Process(%d) try to read %d bytes data from device", currentpid, size);
    // in case the file is accessed from multiple processes/threads
    // TODO here should use mutex instead
    if (down_interruptible(&d_data->sema))
        return -ERESTARTSYS;

    size_t already_read_size = 0;
    if (0 >= size) goto release;

    PDevData p = d_data;

    size_t ready_size = 0;


recheck_if_has_data:
    atomic_set(&p->waiting_for_read, 1);
    // keep waiting until there is some data in the buffer
    size_t data_size = dbuffer_contains_data(p->p_buff);
    if (data_size < 0) {
        // no more data to read, just return
        goto release;
    } else if (0 == data_size) {
        // need to wait
        wait_event_interruptible_exclusive(p->read_queue, 
                atomic_read(&p->waiting_for_read) == 0);
        if (signal_pending(current)) {
            D(TAG, "Process(%d) received singal while waiting for data to read", currentpid);
            already_read_size = -ERESTARTSYS;
            goto release;
        }
        goto recheck_if_has_data;
    }

    // keep going, as there is some data in the buffer
    D(TAG, "There are %d bytes of data in the buffer for read", ready_size);
    already_read_size = read_from_dbuffer_to_user(p->p_buff, buff, size);

    *offset += already_read_size;

release:
    up(&d_data->sema);

    return already_read_size;
}

static loff_t device_llseek(struct file *filep, loff_t offset, int whence)
{
    return -EINVAL;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .read = device_read,
    .llseek = device_llseek,
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
    init_mem_cache();

    // allocate memory to store data
    d_data = (PDevData) alloc_mem(sizeof(DevData));
    if (!d_data) {
        ret = -ENOMEM;
        E(D_NAME, "failed to allocate memory to store data");
        goto error_with_major;
    }
    memset(d_data, 0, sizeof(DevData));
    d_data->current_pid = -1;
    atomic_set(&d_data->waiting_for_read, 0);
    init_waitqueue_head(&d_data->wait_queue);
    init_waitqueue_head(&d_data->read_queue);

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

    d_data->p_buff = create_new_dbuffer();
    if (!d_data->p_buff) {
        ret = -EINVAL;
        E(TAG, "Unable to create page buffer");
        goto error_with_device;
    }

    d_data->c_buff = create_new_cbuffer(C_BUFFER_SIZE);
    if (!d_data->c_buff) {
        ret = -EINVAL;
        E(TAG, "Unable to create circular buffer");
        goto error_with_pbuffer;
    }

    d_data->reader = create_new_gpio_reader(read_trigger);
    if (!d_data->reader) {
        ret = -EINVAL;
        E(TAG, "Unable to create gpio reader");
        goto error_with_cbuffer;
    }
    tasklet_init(&d_data->cbuffer_tasklet, migration_tasklet, 0);
    

    return 0;

error_with_cbuffer:
    release_cbuffer(d_data->c_buff);

error_with_pbuffer:
    release_dbuffer(d_data->p_buff);

error_with_device:
    device_destroy(d_data->clazz, dev_no);

error_with_class:
    class_destroy(d_data->clazz);

error_with_cdev:
    cdev_del(&d_data->dev);

error_with_data:
    kfree(d_data);

error_with_major:
    release_major_number(dev_no);

    release_mem_cache();
    return ret;
}

static void __exit asgn2_exit(void)
{
    I(D_NAME, "Byte, module unloaded at 0x%p\n", asgn2_exit);

    tasklet_kill(&d_data->cbuffer_tasklet);
    release_gpio_reader(d_data->reader);
    release_cbuffer(d_data->c_buff);
    release_dbuffer(d_data->p_buff);
    dev_t dev_no = MKDEV(major, 0);    
    device_destroy(d_data->clazz, dev_no);
    class_destroy(d_data->clazz);
    cdev_del(&d_data->dev);
    release_mem((void *) d_data);
    release_major_number(dev_no);
    release_mem_cache();
}

module_init(asgn2_init);
module_exit(asgn2_exit);
