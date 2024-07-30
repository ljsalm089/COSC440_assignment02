#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/kdev_t.h>
#include <linux/slab.h> // For kmalloc
#include <linux/gfp.h> // for get_zeroed_page
#include <linux/string.h>
#include <linux/uaccess.h> // copy_from_user copy_to_user
#include <linux/semaphore.h> // for struct semaphore and corresponding function 

#include "common.h"

#define D_NAME "assignment02"
#define C_NAME "assignment_class"

#define MIN(l, r) (l) < (r) ? (l) : (r)
#define MAX(l, r) (l) > (r> ? (l) : (r)

static int major = 0;
module_param(major, int, S_IRUGO);

MODULE_PARM_DESC(major, "device major number");
MODULE_AUTHOR("Jiasheng Li");
MODULE_LICENSE("GPL");

// define structure to save the allocated memory
typedef struct page_node 
{
    void * page;
    struct page_node * next;
    struct page_node * prev;
} PageNode;
typedef PageNode * PNode;

// define structure to save the read and write possition
typedef struct position
{
    PNode pindex;
    size_t offset;
} Position;
typedef Position * Pos;

typedef struct dev_data
{
    struct class *clazz;
    struct device *device;
    struct cdev dev;

    struct semaphore sema;

    // read position
    Position rpos;
    // write position
    Position wpos;
} DevData;
typedef DevData * PDevData;

// declare data for module
static PDevData d_data;

static PNode alloc_new_page(void)
{
    PNode new_node = (PNode) kmalloc(sizeof(PageNode), GFP_KERNEL);
    if (!new_node)
    {
        E(D_NAME, "unabled to allocate memory for node");
        return NULL;
    }
    memset(new_node, 0, sizeof(PageNode));

    new_node->page = (void *) get_zeroed_page(GFP_KERNEL);
    if (!new_node->page)
    {
        E(D_NAME, "unabled to allocate new page");
        kfree(new_node);
        return NULL;
    }
    return new_node;
}

static void free_page_with_node(PNode pnode)
{
    free_page((unsigned long) pnode->page);
    kfree(pnode);
}
    
static int device_open(struct inode *node, struct file *filep)
{
    I(D_NAME, "open the file");
    return 0;
} 
    
static int device_release(struct inode *node, struct file *filep)
{
    I(D_NAME, "close the file");
    return 0;
}

static ssize_t device_read(struct file * filep, char * buff, size_t size, loff_t * offset)
{
    if (down_interruptible(&d_data->sema))
        return -ERESTARTSYS;

    PDevData p = d_data;
    size_t already_read_size = 0;

    D(D_NAME, "start to read %ld bytes from the file\n", size);
    if (!p->rpos.pindex)
    {
        D(D_NAME, "no data to read");
        goto release;
    }

    if (p->rpos.pindex == p->wpos.pindex
            && p->rpos.offset >= p->wpos.offset)
    {
        D(D_NAME, "no data to read");
        goto release;
    }

    // there are some contents for reading
    ssize_t read_size = 0;
    while (already_read_size < size /* not enough yet */
                && (p->rpos.pindex != p->wpos.pindex /* reading and writing not in same page*/
                        || p->rpos.offset < p->wpos.offset /* reading offset smaller than writing offset */))
    {
        int expected_size = size - already_read_size;
        read_size = p->rpos.pindex == p->wpos.pindex ? MIN(p->wpos.offset - p->rpos.offset, expected_size) 
            : MIN(PAGE_SIZE - p->rpos.offset, expected_size);

        D(D_NAME, "expect to read %d of content from kernel space %p to user space %p", expected_size,
                p->rpos.pindex->page + p->rpos.offset, buff + already_read_size);
        if(copy_to_user(buff + already_read_size, p->rpos.pindex->page + p->rpos.offset, read_size))
        {
            already_read_size = -EFAULT;
            goto release;
        }
        p->rpos.offset += read_size;
        already_read_size += read_size;

        if (p->rpos.offset == PAGE_SIZE) 
        {
            D(D_NAME, "already finish one page reading");
            PNode current_node = p->rpos.pindex;
            if (NULL == current_node->next)
                break;

            p->rpos.pindex = current_node->next;
            p->rpos.offset = 0;
            p->rpos.pindex->prev = NULL;

            D(D_NAME, "release the old page at address: %p", current_node->page);
            free_page_with_node(current_node); 
        }
    }

release:
    up(&d_data->sema);

    return already_read_size;
}


static ssize_t device_write(struct file * filep, const char * buff, size_t size, loff_t * offset)
{
    I(D_NAME, "write %ld bytes to the file\n", size);
    size_t ret = size;
    size_t left = size;
    PDevData p = d_data;

    if (down_interruptible(&p->sema))
        return -ERESTARTSYS;

    while (left > 0) 
    {
        // no valid page yet, allocate a new page
        if (NULL == p->wpos.pindex) 
        {
            D(D_NAME, "no valided page, try to allocate a new page");
            PNode new_page = alloc_new_page();
            if (NULL == new_page)
            {
                E(D_NAME, "allocate new page error");
                ret = -ENOMEM;
                goto release;
            }
            p->rpos.pindex = new_page;
            p->wpos.pindex = new_page;
            p->rpos.offset = 0;
            p->wpos.offset = 0;
            D(D_NAME, "allocate new page success, with page address: 0x%p", p->wpos.pindex->page);
        }
        
        size_t this_write_size = MIN(PAGE_SIZE - p->wpos.offset, left);
        D(D_NAME, "size of the content that expected to write: %ld", this_write_size);

        // write this_write_size first
        if(copy_from_user(p->wpos.pindex->page + p->wpos.offset, buff + (size - left), this_write_size))
        {
            ret = -EFAULT;
            goto release;
        }
        p->wpos.offset += this_write_size;
        left -= this_write_size;

        if (p->wpos.offset == PAGE_SIZE)
        {
            // already used a whole page, allocate a new page
            D(D_NAME, "already exhaust one page, allocate a new one");
            PNode new_page = alloc_new_page();
            if (NULL == new_page)
            {
                E(D_NAME, "failed to allocate a new page");
                ret = -ENOMEM;
                goto release;
            }
            D(D_NAME, "allocate new page success, with page address: 0x%p", p->wpos.pindex->page);
            new_page->next = NULL;
            new_page->prev = p->wpos.pindex;
            p->wpos.pindex->next = new_page;
            p->wpos.pindex = new_page;
            p->wpos.offset = 0;
        }
    }

release:
    up(&p->sema);

    return size;
}

static long device_ioctl(struct file * filep, unsigned int cmd, unsigned long arg)
{
    return 0;
}

static struct file_operations fops =
{
    .open = device_open,
    .read = device_read,
    .write = device_write,
    .release = device_release,
    .unlocked_ioctl = device_ioctl,
};

static int allocate_major_number(dev_t *devno, int *major)
{
    int ret = -1;
    D(D_NAME, "Try to register for major number: %d", *major);
    *devno = MKDEV(*major, 0);
    if (*devno) 
        ret = register_chrdev_region(*devno, 1, D_NAME);

    if (ret < 0)
    {
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
    d_data = kmalloc(sizeof(DevData), GFP_KERNEL);
    if (!d_data)
    {
        ret = -ENOMEM;
        E(D_NAME, "failed to allocate memory to store data");
        goto error_with_major;
    }
    memset(d_data, 0, sizeof(DevData));

    // initialise the semaphore
    sema_init(&d_data->sema, 1);

    // initialise dev
    cdev_init(&d_data->dev, &fops);
    d_data->dev.owner = THIS_MODULE;

    ret = cdev_add(&d_data->dev, dev_no, 1);
    if (ret < 0)
    {
        E(D_NAME, "failed to add cdev");
        goto error_with_data;
    }

    D(D_NAME, "add cdev successfully");

    // create class
    d_data->clazz = class_create(THIS_MODULE, C_NAME);
    if (IS_ERR(d_data->clazz))
    {
        ret = PTR_ERR(d_data->clazz);
        E(D_NAME, "failed to create class(%s) for device: %d", C_NAME, ret);
        goto error_with_cdev;
    }
    D(D_NAME, "create class(%s) successfully", C_NAME);

    // create device
    d_data->device = device_create(d_data->clazz, NULL, dev_no, NULL, D_NAME);
    if (IS_ERR(d_data->device))
    {
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

static void __exit my_exit(void)
{
    I(D_NAME, "Byte, module unloaded at 0x%p\n", my_exit);
    dev_t dev_no = MKDEV(major, 0);    
    device_destroy(d_data->clazz, dev_no);
    class_destroy(d_data->clazz);
    cdev_del(&d_data->dev);
    kfree(d_data);
    release_major_number(dev_no);
}

module_init(my_init);
module_exit(my_exit);
