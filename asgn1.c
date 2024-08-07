#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/slab.h> // For `kmalloc`
#include <linux/gfp.h> // for `get_zeroed_page`
#include <linux/string.h>
#include <linux/uaccess.h> // `copy_from_user` `copy_to_user`
#include <linux/semaphore.h> // for struct `semaphore` and corresponding function 
#include <linux/wait.h>   // for `wait_queue_head_t`
#include <linux/sched.h> // for macro `current` to get current process info
#include <linux/atomic.h> // for type `atomic_t` and operation functions
#include <linux/kobject.h>  // for structure kobject and relevant function
#include <linux/mm.h> // for remap_pfn_range
#include <linux/spinlock.h> // for spinlock_t and related functions

#include "common.h"

#define D_NAME "asgn1"
#define C_NAME "assignment_class"

#define MIN(l, r) (l) < (r) ? (l) : (r)
#define MAX(l, r) (l) > (r) ? (l) : (r)

static int major = 0;
module_param(major, int, S_IRUGO);

static int max_process_count = 1;
module_param(max_process_count, int, S_IRUGO);

MODULE_PARM_DESC(major, "device major number");
MODULE_AUTHOR("Jiasheng Li");
MODULE_LICENSE("GPL");

// define structure to save the allocated memory
typedef struct mem_node {
    struct list_head node;
    void * page;
    size_t index;
} MemNode;

typedef MemNode * PMemNode;

typedef struct dev_data {
    struct class *clazz;
    struct device *device;
    struct cdev dev;

    struct semaphore sema;

    struct list_head mem_list;
    ssize_t file_size;

    wait_queue_head_t wait_queue;

    int count;
    spinlock_t count_lock;
    unsigned long lock_flags;
} DevData;
typedef DevData * PDevData;

// declare data for module
static PDevData device_data;

// function to change the access permissions of the deivce file
static char *asgn1_class_devnode(struct device *dev, umode_t *mode)
{
    if (mode)
        *mode = 0666;
    return NULL;
}

// function to allocate a new page and node to manage it
static PMemNode alloc_new_page(void)
{
    PMemNode new_node = (PMemNode) kmalloc(sizeof(MemNode), GFP_KERNEL);
    if (!new_node) {
        E(D_NAME, "unabled to allocate memory for node");
        return NULL;
    }
    memset(new_node, 0, sizeof(MemNode));

    new_node->page = (void *) get_zeroed_page(GFP_KERNEL);
    if (!new_node->page) {
        E(D_NAME, "unabled to allocate new page");
        kfree(new_node);
        return NULL;
    }
    return new_node;
}

// function to free the allocated page and node
static void free_page_with_node(PMemNode pnode)
{
    free_page((unsigned long) pnode->page);
    kfree(pnode);
}

static void try_to_wake_up_processes(void)
{
    spin_lock_irqsave(&device_data->count_lock, device_data->lock_flags);
    int try_to_wake_number = max_process_count - device_data->count;
    spin_unlock_irqrestore(&device_data->count_lock, device_data->lock_flags);
    if (try_to_wake_number > 0) {
        wake_up_interruptible_nr(&device_data->wait_queue, try_to_wake_number);
    }
}
    
static int device_open(struct inode *node, struct file *filep)
{
    D(D_NAME, "process(%d) try to open the device", currentpid);
recheck:
    spin_lock_irqsave(&device_data->count_lock, device_data->lock_flags);
    if (device_data->count < max_process_count) {
        device_data->count ++;
        spin_unlock_irqrestore(&device_data->count_lock, device_data->lock_flags);
    } else {
        spin_unlock_irqrestore(&device_data->count_lock, device_data->lock_flags);
        D(D_NAME, "No resource, start waiting: %d", currentpid);
        wait_event_interruptible_exclusive(device_data->wait_queue, 
                device_data->count < max_process_count);
        D(D_NAME, "Process(%d) is awake, check what happen", currentpid);
        if (signal_pending(current))
            return -ERESTARTSYS;
        goto recheck;
    }

    D(D_NAME, "Process(%d) has gained the resource", currentpid);
    return 0;
} 
    
static int device_release(struct inode *node, struct file *filep)
{
    D(D_NAME, "Process(%d) close the device", currentpid);
    spin_lock_irqsave(&device_data->count_lock, device_data->lock_flags);
    device_data->count --;
    int try_to_wake_number = max_process_count - device_data->count;
    spin_unlock_irqrestore(&device_data->count_lock, device_data->lock_flags);

    if (try_to_wake_number > 0) 
        wake_up_interruptible_nr(&device_data->wait_queue, try_to_wake_number);
    return 0;
}

static loff_t expand_edge(loff_t expected_size)
{
    size_t current_cache_size = list_empty(&device_data->mem_list) 
        ? 0 : ((list_last_entry(&device_data->mem_list, MemNode, node)->index + 1) * PAGE_SIZE);

    D(D_NAME, "current cache size: %ld, and expected size: %lld", current_cache_size, expected_size);

    if (current_cache_size > expected_size) 
        return current_cache_size;

    // no enough space, expand it
    while (current_cache_size <= expected_size) {
        size_t index = 0 == current_cache_size 
            ? 0 :list_last_entry(&device_data->mem_list, MemNode, node)->index + 1;
        PMemNode new_node = alloc_new_page();
        if (NULL == new_node) {
            E(D_NAME, "failed to allocate new page");
            return -1;
        }
        new_node->index = index;
        list_add_tail(&new_node->node, &device_data->mem_list);
        D(D_NAME, "extend one page successfully");
        current_cache_size += PAGE_SIZE;
    }
    return current_cache_size;
}

static loff_t device_llseek(struct file *filep, loff_t offset, int whence)
{
    loff_t new_pos;
    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = filep->f_pos + offset;
            break;
        case SEEK_END:
            // can not seek the position beyond the file size.
            new_pos = device_data->file_size + offset;
            break;
        default:
            return -EINVAL;
    }
    if (new_pos < 0 || new_pos > device_data->file_size) 
        return -EINVAL;
    filep->f_pos = new_pos;
    return filep->f_pos;
}

static ssize_t device_read(struct file * filep, char * buff, size_t size, loff_t * offset)
{
    // in case the file is accessed from multiple threads
    if (down_interruptible(&device_data->sema))
        return -ERESTARTSYS;

    PDevData p = device_data;
    size_t already_read_size = 0;

    D(D_NAME, "Process(%d) start to read %ld bytes from the file\n", currentpid, size);
    if (list_empty(&p->mem_list)) {
        D(D_NAME, "no data to read");
        goto release;
    }

    if (device_data->file_size <= *offset) {
        D(D_NAME, "no data to read");
        goto release;
    }

    // in case there is no enough data to read
    size_t expected_read_size = MIN(device_data->file_size - *offset, size);

    // read data from the offset to the buffer
    size_t start_node = *offset / PAGE_SIZE;
    size_t node_index = 0;
    size_t page_offset = 0;

    struct list_head *ptr;
    PMemNode curr;
    list_for_each(ptr, &p->mem_list) {
        curr = list_entry(ptr, MemNode, node);
        if (node_index >= start_node) {
            // read from some offset from the first page, and read from 0 from other pages
            page_offset = node_index == start_node ? *offset % PAGE_SIZE : 0; 
            size_t read_size = MIN(PAGE_SIZE - page_offset, expected_read_size - already_read_size);
            if (copy_to_user(buff + already_read_size, curr->page + page_offset, read_size)) {
                already_read_size = -EFAULT;
                goto release;
            }
            already_read_size += read_size;

            if (already_read_size == expected_read_size)
                break;
        }
        node_index ++;
    }
    *offset += already_read_size;

release:
    up(&device_data->sema);

    return already_read_size;
}


static ssize_t device_write(struct file * filep, const char * buff, size_t size, loff_t * offset)
{
    I(D_NAME, "Process(%d) start to write %ld bytes to the file\n", currentpid, size);
    size_t left = size;
    PDevData p = device_data;

    // in case the file is accessed from multiple threads
    if (down_interruptible(&p->sema))
        return -ERESTARTSYS;

    // expand the cache size first, make sure there is enough space to write
    if (expand_edge(*offset + 1 + size) < 0) {
        D(D_NAME, "failed to expand the cache size");
        size = -ENOMEM;
        goto release;
    }

    size_t node_index = 0;
    size_t start_node = *offset / PAGE_SIZE;
    struct list_head *ptr;
    PMemNode curr;
    list_for_each(ptr, &p->mem_list) {
        curr = list_entry(ptr, MemNode, node);
        if (node_index >= start_node) {
            // write from some offset to the first page, and write from 0 to other pages
            int page_offset = node_index == start_node ? *offset % PAGE_SIZE : 0;
            size_t write_size = MIN(PAGE_SIZE - page_offset, left);
            D(D_NAME, "write %ld bytes data to page %ld", write_size, node_index);
            if (copy_from_user(curr->page + page_offset, buff + (size - left), write_size)) {
                size = -EFAULT;
                goto release;
            }
            left -= write_size;

            if (left == 0)
                break;
        }
        node_index ++;
    }
    *offset += size;

    // reset the file size to new value
    p->file_size = MAX(p->file_size, *offset + 1);


release:
    up(&p->sema);

    return size;
}

static int change_max_process(int count) 
{
    if (count < 1) {
        return -EINVAL;
    }
    max_process_count = count;
    try_to_wake_up_processes();
    return 0;
}

#define IOCTL_DEMO_COMMAND _IO('D', 1)
#define IOCTL_MAX_COMMAND _IOR('M', 2, int)

static long device_ioctl(struct file * filep, unsigned int cmd, unsigned long arg)
{
    D(D_NAME, "call ioctl on the device, with command: %d", cmd);
    switch (cmd) {
        case IOCTL_DEMO_COMMAND:
            I(D_NAME, "The demo command is called!");
            break;
        case IOCTL_MAX_COMMAND:
            int max_process;
            if (copy_from_user(&max_process, (void __user *) arg, sizeof(int)))
                // fail to read data from user space
                return -EFAULT;
            return change_max_process(max_process);
        default:
            // Not a valid ioctl command
            return -ENOTTY;
    }
    return 0;
}

static int device_mmap(struct file *filep, struct vm_area_struct * vm_area)
{
    int ret = 0;  // means mapping successfully

    if (down_interruptible(&device_data->sema))
        return -ERESTARTSYS;

    size_t mmap_size = vm_area->vm_end - vm_area->vm_start;
    D(D_NAME, "expected mapping size: %ld", mmap_size);
    unsigned long pfn;

    int start_mapping_index = vm_area->vm_pgoff;
    D(D_NAME, "start mapping page index: %d", start_mapping_index);
    loff_t target_file_size = (start_mapping_index << PAGE_SHIFT) + mmap_size;
    D(D_NAME, "target expanded size: %lld", target_file_size);

    if (expand_edge(start_mapping_index * PAGE_SIZE + mmap_size) < 0) {
        E(D_NAME, "Failed to expand the size");
        ret = -EAGAIN;
        goto release_sema;
    }

    int node_index = 0;

    size_t already_mapped_size = 0;
    
    struct list_head *ptr;
    PMemNode curr;
    list_for_each(ptr, &device_data->mem_list) {
        curr = list_entry(ptr, MemNode, node);
        if (node_index >= start_mapping_index) {
            D(D_NAME, "Start to map page with index: %d", node_index);
            pfn = virt_to_phys(curr->page) >> PAGE_SHIFT;

            if (remap_pfn_range(vm_area, vm_area->vm_start + already_mapped_size, pfn, 
                        PAGE_SIZE, vm_area->vm_page_prot)) {
                E(D_NAME, "Failed to map memory, and the page index is: %d", node_index);
                ret = -EAGAIN;
                goto release_sema;
            }

            already_mapped_size += PAGE_SIZE;
        }
        node_index ++;

        // extend the file size
        device_data->file_size = MAX(device_data->file_size, node_index * PAGE_SIZE);

        if (already_mapped_size >= mmap_size)
            break;
    }
    D(D_NAME, "Map all the expected pages successfully");

release_sema:
    up(&device_data->sema);

    return ret; 
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .read = device_read,
    .write = device_write,
    .release = device_release,
    .unlocked_ioctl = device_ioctl,
    .llseek = device_llseek,
    .mmap = device_mmap,
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

static int __init my_init(void)
{
    int ret;
    I(D_NAME, "Hello, module loaded at 0x%p", my_init);
    I(D_NAME, "The value of parameter major is %d", major);

    dev_t dev_no;

    // allocate major number for device
    ret = allocate_major_number(&dev_no, &major);
    if (ret < 0) return ret; 

    D(D_NAME, "registered correctly with major number: %d", major);

    // allocate memory to store data
    device_data = kmalloc(sizeof(DevData), GFP_KERNEL);
    if (!device_data) {
        ret = -ENOMEM;
        E(D_NAME, "failed to allocate memory to store data");
        goto error_with_major;
    }
    memset(device_data, 0, sizeof(DevData));
    INIT_LIST_HEAD(&device_data->mem_list);
    init_waitqueue_head(&device_data->wait_queue);

    // initialise the semaphore
    sema_init(&device_data->sema, 1);

    // initialise the spinlock
    spin_lock_init(&device_data->count_lock);

    // initialise dev
    cdev_init(&device_data->dev, &fops);
    device_data->dev.owner = THIS_MODULE;

    ret = cdev_add(&device_data->dev, dev_no, 1);
    if (ret < 0) {
        E(D_NAME, "failed to add cdev");
        goto error_with_data;
    }

    D(D_NAME, "add cdev successfully");

    // create class
    device_data->clazz = class_create(THIS_MODULE, C_NAME);
    if (IS_ERR(device_data->clazz)) {
        ret = PTR_ERR(device_data->clazz);
        E(D_NAME, "failed to create class(%s) for device: %d", C_NAME, ret);
        goto error_with_cdev;
    }
    device_data->clazz->devnode = asgn1_class_devnode;
    D(D_NAME, "create class(%s) successfully", C_NAME);

    // create device
    device_data->device = device_create(device_data->clazz, NULL, dev_no, NULL, D_NAME);
    if (IS_ERR(device_data->device)) {
        ret = PTR_ERR(device_data->device);
        E(D_NAME, "failed to create device: %d", ret);
        goto error_with_class;
    }
    D(D_NAME, "create device successfully");
    I(D_NAME, "initialise successfully");

    return 0;

error_with_class:
    class_destroy(device_data->clazz);

error_with_cdev:
    cdev_del(&device_data->dev);

error_with_data:
    kfree(device_data);

error_with_major:
    release_major_number(dev_no);
    return ret;
}

static void __exit my_exit(void)
{
    I(D_NAME, "Byte, module unloaded at 0x%p\n", my_exit);

    // free all allocated pages
    while (!list_empty(&device_data->mem_list)) {
        PMemNode curr = list_last_entry(&device_data->mem_list,  MemNode, node);
        list_del(&curr->node);

        free_page_with_node(curr);
        D(D_NAME, "Release a page successfully.");
    }

    dev_t dev_no = MKDEV(major, 0);    
    device_destroy(device_data->clazz, dev_no);
    class_destroy(device_data->clazz);
    cdev_del(&device_data->dev);
    kfree(device_data);
    release_major_number(dev_no);
}

module_init(my_init);
module_exit(my_exit);
