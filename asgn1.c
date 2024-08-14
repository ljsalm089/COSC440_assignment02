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
#include <linux/proc_fs.h>  // for struct proc_ops
#include <linux/seq_file.h> // for struct seq_operations and related functions

#include "common.h"

#define D_NAME "asgn1"
#define C_NAME "assignment_class"

#define PROC_CACHE_SIZE 100
#define SEQ_TYPE_MAXIMUM 0x01
#define SEQ_TYPE_TIPS 0x02
#define SEQ_TYPE_PAGE 0x03

#define MIN(l, r) (l) < (r) ? (l) : (r)
#define MAX(l, r) (l) > (r) ? (l) : (r)

static int major = 0;
module_param(major, int, S_IRUGO);

static int max_process_count = 1;
module_param(max_process_count, int, S_IRUGO);

MODULE_PARM_DESC(major, "device major number");
MODULE_AUTHOR("Jiasheng Li");
MODULE_LICENSE("GPL");

typedef struct seq_node {
    struct seq_node * next;
    short type;
    union {
        void * p_val;
        int i_val;
        float f_val;
        char c_val;
    } data;
} SeqNode;

typedef SeqNode * PSeqNode;

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

    struct proc_dir_entry * proc_entry;

    struct semaphore sema;

    struct list_head mem_list;
    ssize_t file_size;

    wait_queue_head_t wait_queue;
    wait_queue_head_t write_only_wait_queue;

    int count;
    int write_only_count;
    spinlock_t count_lock;
} DevData;
typedef DevData * PDevData;

// declare data for module
static PDevData device_data;

// function to change the access permissions of the deivce file
static char *asgn1_class_devnode(const struct device *dev, umode_t *mode)
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
static void free_page_and_node(PMemNode pnode)
{
    // if needs to cache the allocated pages and nodes, 
    // just use a list to link part of the pages and nodes which will be free
    // while need to allocate new page and node, obtain from the list first
    // 
    // here, to simply the code and show the respect for the kernel, just release it
    free_page((unsigned long) pnode->page);
    kfree(pnode);
}

// free all the allocated pages and nodes
static void free_all_pages_and_nodes(void) {
    // free all allocated pages
    while (!list_empty(&device_data->mem_list)) {
        PMemNode curr = list_last_entry(&device_data->mem_list,  MemNode, node);
        list_del(&curr->node);

        free_page_and_node(curr);
        D(D_NAME, "Release a page successfully.");
    }
}

static void try_to_wake_up_processes(void)
{
    spin_lock(&device_data->count_lock);
    int try_to_wake_number = max_process_count - device_data->count;
    spin_unlock(&device_data->count_lock);
    if (try_to_wake_number > 0) {
        // wake up read-write and write-only processes. Who will gain the resource, it's up to the God!
        wake_up_interruptible_nr(&device_data->wait_queue, try_to_wake_number);
        wake_up_interruptible_nr(&device_data->write_only_wait_queue, 1);
    }
}
    
static int device_open(struct inode *node, struct file *filep)
{
    int write_only = filep->f_flags & O_WRONLY;
    D(D_NAME, "process(%d) try to open the device, is write-only: %d", currentpid, write_only);
recheck:
    spin_lock(&device_data->count_lock); 
    // write-only should be exclusive, so no other process should be reading or writing the device
    if ((write_only && device_data->count == 0 && device_data->write_only_count == 0) 
            // not write-only, still need to wait if there is process which is write-only accessing the device 
            // or the read-write processes have reached the limitation
            || (!write_only && 0 == device_data->write_only_count && device_data->count < max_process_count)) {
        if (write_only) {
            device_data->write_only_count ++;
        } else {
            device_data->count ++;
        }
        spin_unlock(&device_data->count_lock);
    } else {
        spin_unlock(&device_data->count_lock);
        D(D_NAME, "No resource, start waiting: %d", currentpid);
        if (write_only) {
            wait_event_interruptible_exclusive(device_data->write_only_wait_queue,
                    device_data->count == 0 && device_data->write_only_count == 0);
        } else {
            wait_event_interruptible_exclusive(device_data->wait_queue, 
                    device_data->count < max_process_count && device_data->write_only_count == 0);
        }
        D(D_NAME, "Process(%d) is awake, check what happen", currentpid);
        if (signal_pending(current))
            return -ERESTARTSYS;
        goto recheck;
    }

    if (write_only) {
        // write only, clear all data before writing
        free_all_pages_and_nodes();
        device_data->file_size = 0;
    }

    D(D_NAME, "Process(%d) has gained the resource", currentpid);
    return 0;
} 
    
static int device_release(struct inode *node, struct file *filep)
{
    int write_only = filep->f_flags & O_WRONLY;
    D(D_NAME, "Process(%d) close the device, is write-only: %d", currentpid, write_only);
    spin_lock(&device_data->count_lock);
    if (write_only) {
        device_data->write_only_count --;
    } else {
        device_data->count --;
    }
    int try_to_wake_number = max_process_count - device_data->count;
    spin_unlock(&device_data->count_lock);

    if (try_to_wake_number > 0) {
        // wake up read-write and write-only processes. Who will gain the resource, it's up to the God!
        wake_up_interruptible_nr(&device_data->wait_queue, try_to_wake_number);
        wake_up_interruptible_nr(&device_data->write_only_wait_queue, 1);
    }
    return 0;
}

static loff_t expand_edge(loff_t expected_size)
{
    size_t current_cache_size = list_empty(&device_data->mem_list) 
        ? 0 : ((list_last_entry(&device_data->mem_list, MemNode, node)->index + 1) * PAGE_SIZE);

    D(D_NAME, "current cache size: %d, and expected size: %lld", current_cache_size, expected_size);

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

    D(D_NAME, "Process(%d) start to read %d bytes from the file\n", currentpid, size);
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
    I(D_NAME, "Process(%d) start to write %d bytes to the file\n", currentpid, size);
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
            D(D_NAME, "write %d bytes data to page %d", write_size, node_index);
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
    D(D_NAME, "expected mapping size: %d", mmap_size);
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

static void * asgn_proc_seq_start(struct seq_file *filep, loff_t *pos) {
    PSeqNode node = (PSeqNode) kmalloc(sizeof(SeqNode), GFP_KERNEL);
    memset(node, 0, sizeof(SeqNode));
    ((void **) filep->private)[0] = (void *) node;
    node->type = SEQ_TYPE_MAXIMUM;
    node->data.p_val= &max_process_count;

    if (!list_empty(&device_data->mem_list)) {
        PMemNode mem_node = list_last_entry(&device_data->mem_list, MemNode, node);
        node->next = (PSeqNode) kmalloc(sizeof(SeqNode), GFP_KERNEL);
        node = node->next;
        memset(node, 0, sizeof(SeqNode));
        node->type = SEQ_TYPE_TIPS;
        node->data.i_val = mem_node->index + 1;

        struct list_head *ptr;
        PMemNode curr;

        list_for_each(ptr, &device_data->mem_list) {
            curr = list_entry(ptr, MemNode, node);

            node->next = (PSeqNode) kmalloc(sizeof(SeqNode), GFP_KERNEL);
            node = node->next;
            memset(node, 0, sizeof(SeqNode));
            node->type = SEQ_TYPE_PAGE;
            node->data.p_val = curr;
        }
    }

    loff_t index = 0;
    node = ((void **) filep->private)[0];
    while (NULL != node && index <= *pos) {
        if (index == *pos) {
            return node;
        }
        node = node->next;
        index += 1;
    }

    return NULL;
}

static void asgn_proc_seq_stop(struct seq_file *filep, void *v) {
    PSeqNode next, node = ((void **) filep->private)[0];

    while (NULL != node) {
        next = node->next;
        kfree(node);
        node = next;
    }
}

static void * asgn_proc_seq_next(struct seq_file *filep, void *v, loff_t *pos) {
    PSeqNode node = (PSeqNode) v;
    if (NULL == node || NULL == node->next) return NULL;

    *pos += 1;
    return (void *)node->next;
}

static int asgn_proc_seq_show(struct seq_file *filep, void *v) {
    if (NULL == v) return -EINVAL;

    PSeqNode node = (PSeqNode) v;
    switch (node->type) {
        case SEQ_TYPE_MAXIMUM:
            seq_printf(filep, "Maximum process accessing count: %d\n", 
                    * ((int *)node->data.p_val));
            break;
        case SEQ_TYPE_TIPS:
            seq_printf(filep, "Already allocated pages (%d):\n", node->data.i_val);
            break;
        case SEQ_TYPE_PAGE:
            PMemNode mem_node = (PMemNode) node->data.p_val;
            seq_printf(filep, "Page %d: from 0x%p to 0x%p\n", mem_node->index, 
                    mem_node->page, mem_node->page + PAGE_SIZE);
            break;
        default:
            seq_printf(filep, "Unknown information, type: %d\n", (int) node->type);
            break;
    }
    return 0;
}

static struct seq_operations asgn_seq_ops = {
    .start = asgn_proc_seq_start,
    .stop = asgn_proc_seq_stop,
    .next = asgn_proc_seq_next,
    .show = asgn_proc_seq_show,
};

static int asgn_proc_open(struct inode * nodep, struct file *filep) {
    int ret;
    void * cache = NULL;
    void ** private = (void **) kmalloc(2 * sizeof(void *), GFP_KERNEL);

    if (IS_ERR(private)) {
        ret = PTR_ERR(private);
        E(D_NAME, "Unable to allocate memory for reading and writing: %d", ret);
        goto asgn_proc_open_return;
    }
    memset(private, 0, 2 * sizeof(void *));
        

    // if open the file as writable, allocate buffer for writing
    if (filep->f_flags & O_RDWR || filep->f_flags & O_WRONLY) {
        cache = kmalloc(PROC_CACHE_SIZE, GFP_KERNEL);
        if (IS_ERR(cache)) {
            ret = PTR_ERR(cache);
            E(D_NAME, "Unable to allocate memory for writing: %d", ret);
            goto asgn_proc_open_release_private;
        }
        memset(cache, 0, PROC_CACHE_SIZE);
        D(D_NAME, "Address of allocated cache for writing: 0x%p", cache);
    }

    ret = seq_open(filep, &asgn_seq_ops);

    if (0 != ret) {
        E(D_NAME, "Failed to initilise for sequential reading: %d", ret);
        goto asgn_proc_open_release_cache;
    }

    struct seq_file * p_seq_file = (struct seq_file *) filep->private_data;
    // private points to an array, the first element points to sequence for reading
    // the second element points to cache for writing
    private[0] = p_seq_file->private;
    private[1] = cache;
    p_seq_file->private = private;

    return ret;

asgn_proc_open_release_cache:
    if (NULL != cache) kfree(cache);

asgn_proc_open_release_private: 
    kfree(private);

asgn_proc_open_return:
    return ret;
}

static int asgn_proc_release(struct inode * inodep, struct file * filep) {
    struct seq_file * p_seq_file = filep->private_data;
    
    void ** private = (void **) p_seq_file->private;
    void * cache_for_writing = private[1];

    int ret = seq_release(inodep, filep);

    if (NULL != cache_for_writing) {
        // parse and execute the command in cache before releasing it
        char * max_param_prefix = "max_process_count=";
        char *ptr;
        int prefix_length = strlen(max_param_prefix);
        if (strncmp(max_param_prefix, cache_for_writing, prefix_length) == 0) {
            long max_count = simple_strtol(cache_for_writing + prefix_length, &ptr, 10);

            if (max_count > 0) {
                // change the maximum number of process to access the file concurrently
                change_max_process(max_count);
            } else {
                D(D_NAME, "Invalid max_process_count, ignore it: %d", max_process_count);
            }
        }
        kfree(cache_for_writing);
    }
    if (NULL != private) kfree(private);

    return ret;
}

static ssize_t asgn_proc_write(struct file *filep, const char __user * buff, 
        size_t size, loff_t *pos) {
    if (*pos >= PROC_CACHE_SIZE || *pos < 0) return -EINVAL;

    D(D_NAME, "Write %d bytes from buffer to cache: %s", size, buff); 

    struct seq_file * p_seq_file = filep->private_data;
    void *cache = ((void **)p_seq_file->private)[1];

    size_t write_size = MIN(size, PROC_CACHE_SIZE - *pos);
    D(D_NAME, "Expected writing size: %d, target writing address: 0x%p", write_size, cache);
    int ret = copy_from_user(cache + (*pos), buff, write_size);
    if (0 != ret) {
        E(D_NAME, "Failed to copy data from user space: %d\n", ret);
        return ret;
    }
    return write_size;
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

static struct proc_ops proc_entity_ops = {
    .proc_open = asgn_proc_open,
    .proc_lseek = seq_lseek,
    .proc_read = seq_read,
    .proc_release = asgn_proc_release,
    .proc_write = asgn_proc_write,
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
    init_waitqueue_head(&device_data->write_only_wait_queue);

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
    device_data->clazz = class_create(C_NAME);
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

    // create entry in folder /proc
    device_data->proc_entry = proc_create(D_NAME, 0644, NULL, &proc_entity_ops);
    if (IS_ERR(device_data->proc_entry)) {
        ret = PTR_ERR(device_data->proc_entry);
        E(D_NAME, "failed to create entry in '/proc': %d", ret);
        goto error_with_device;
    }

    return 0;
error_with_device:
    device_destroy(device_data->clazz, dev_no);

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

    free_all_pages_and_nodes();

    remove_proc_entry(D_NAME, NULL);
    dev_t dev_no = MKDEV(major, 0);    
    device_destroy(device_data->clazz, dev_no);
    class_destroy(device_data->clazz);
    cdev_del(&device_data->dev);
    kfree(device_data);
    release_major_number(dev_no);
}

module_init(my_init);
module_exit(my_exit);
