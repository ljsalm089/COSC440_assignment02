# include <linux/kernel.h>
# include <linux/gfp.h>  // for `get_zeroed_page`
# include <linux/spinlock.h> // for spinlock_t and related functions

# include "common.h"
# include "mem_cache.h"


#define TAG "MCache"


// the struct is allocated at the beginning of the page
// after being created, there are at least 2 AllocatedRegions in the `sub_list`
typedef struct {
    void * page;
    
    ListHead sub_list;

    ListHead node;
} CacheNode;

typedef CacheNode * PCNode;

// the allocated region in the page, all this structure should be stored in the page
typedef struct {
    unsigned int allocated_size;
    unsigned int usage_size;
    ListHead node;
    void * start_addr;
} AllocatedRegion;

typedef AllocatedRegion * PARegion;


// global variable to store the allocated pages
static ListHead cache_nodes;

// spin lock to protect while allocating and releasing memory
static spinlock_t lock;

PCNode _allocate_new_cache_node(void)
{
    void * page = (void *) get_zeroed_page(GFP_KERNEL);
    if (page) {
        PCNode node = (PCNode) page;
        INIT_LIST_HEAD(&node->sub_list);
        node->page = page;

        PARegion start = page + sizeof(CacheNode);
        start->allocated_size = sizeof(AllocatedRegion);
        start->usage_size = 0;
        start->start_addr = (void *) start;
        list_add_tail(&start->node, &node->sub_list);

        PARegion end = page + PAGE_SIZE - sizeof(CacheNode);
        end->allocated_size = sizeof(AllocatedRegion);
        end->usage_size = 0;
        end->start_addr = (void *) end;
        list_add_tail(&end->node, &node->sub_list);
        return node;
    }
    return NULL;
}

int init_mem_cache(void)
{
    INIT_LIST_HEAD(&cache_nodes);
    return SUCC;
}

void * _find_available_region_in_page(PCNode page, int size)
{
    // there are at least 2 Region in this list, so it is safe without checking null
    PARegion last = list_first_entry(&page->sub_list, AllocatedRegion, node);
    D(TAG, "Try to find an available region for %d bytes in page: %p", size, page->page);

    PListHead ptr;
    PARegion region;

    list_for_each(ptr, &page->sub_list) {
        region = list_entry(ptr, AllocatedRegion, node);

        if (region->start_addr - (last->start_addr + last->allocated_size) 
                >= size + sizeof(AllocatedRegion)) {
            // find an region which has enough space to hold expected memory and an AllocatedRegion
            PARegion tmp = last->start_addr + last->allocated_size;
            tmp->allocated_size = size + sizeof(AllocatedRegion);
            tmp->usage_size = size;
            tmp->start_addr = (void *) tmp;

            // insert the new region behind the last node, make sure the order is correct
            list_add(&tmp->node, &last->node);

            void * result = tmp->start_addr + sizeof(AllocatedRegion);
            D(TAG, "Found a region: %p", result);
        }
        last = region;
    }
    return NULL;
}

void * alloc_mem(int size)
{
    void * result = NULL;

    spin_lock_wrapper(&lock);

    // the required memory is too large for the module to manege
    if (size > PAGE_SIZE - (sizeof(CacheNode) + 2 * sizeof(AllocatedRegion))) goto release;

    PListHead ptr;
    PCNode curr;

    list_for_each(ptr, &cache_nodes) {
        // check all regions to find a available memory node
        curr = list_entry(ptr, CacheNode, node);
        
        result = _find_available_region_in_page(curr, size);
        if (result) {
            goto release;
        }
    }
    D(TAG, "Didn't find an available region, try to create a new page_node");
    // no available region for new allocation, create a new page
    PCNode page_node = _allocate_new_cache_node();
    if (page_node) {
        list_add_tail(&page_node->node, &cache_nodes);

        result = _find_available_region_in_page(curr, size);
    }
release:
    spin_unlock_wrapper(&lock);
    
    return result;
}

void _release_region_in_page(PCNode page, void *mem)
{
    PListHead ptr;
    PARegion curr;

    list_for_each(ptr, &page->sub_list) {
        curr = list_entry(ptr, AllocatedRegion, node);
        
        if (mem - sizeof(AllocatedRegion) == curr->start_addr) {
            // found the corresponding AllocatedRegion, delete the node from list first
            list_del(&curr->node);

            // clean up this chunk of memory
            memset(curr->start_addr, 0, curr->allocated_size);
            break;
        }
    }
}

void release_mem(void * mem)
{
    if (NULL == mem) return;

    PListHead ptr;
    PCNode curr;
    unsigned long flags;

    if (in_interrupt()) 
        spin_lock_irqsave(&lock, flags);
    else
        spin_lock(&lock);

    list_for_each(ptr, &cache_nodes) {
        curr = list_entry(ptr, CacheNode, node);

        if (mem > curr->page && mem < curr->page + PAGE_SIZE) {
            // the memory should be in this page
            _release_region_in_page(curr, mem);

            // check if there is no any allocated region now, if so, release this page
            // there should be at least 2 entries in the list
            PARegion start = list_first_entry(&curr->sub_list, AllocatedRegion, node);
            PARegion end = list_last_entry(&curr->sub_list, AllocatedRegion, node);
            if (list_next_entry(start, node) == end) {
                // only 2 entries in the list, release this page
                list_del(&curr->node);
                free_page((unsigned long) curr->page);
                break;
            }
        }
    }

    if (in_interrupt())
        spin_unlock_irqrestore(&lock, flags);
    else
        spin_unlock(&lock);
}

void release_mem_cache(void)
{
    while (!list_empty(&cache_nodes)) {
        PCNode tmp_node = list_first_entry_or_null(&cache_nodes, CacheNode, node);

        if (tmp_node) {
            list_del(&tmp_node->node);
            free_page((unsigned long) tmp_node->page);
        }
    }
}

