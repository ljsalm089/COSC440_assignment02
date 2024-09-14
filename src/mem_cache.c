# include <linux/kernel.h>
# include <linux/gfp.h>  // for `get_zeroed_page`
# include <linux/spinlock.h> // for spinlock_t and related functions

# include "common.h"
# include "mem_cache.h"


#ifndef DEBUG
// #define DEBUG_M
#endif
#define TAG "MCache"

#define P2L(r) ((unsigned long) (r))


// the struct is allocated at the beginning of the page
// after being created, there are at least 2 AllocatedRegions in the `sub_list`
typedef struct {
    unsigned long page;
    
    ListHead sub_list;

    ListHead node;
} CacheNode;

typedef CacheNode * PCNode;

// the allocated region in the page, all this structure should be stored in the page
typedef struct {
    unsigned int allocated_size;
    unsigned int usage_size;
    ListHead node;
    unsigned long start_addr;
} AllocatedRegion;

typedef AllocatedRegion * PARegion;


// global variable to store the allocated pages
static ListHead cache_nodes;

// spin lock to protect while allocating and releasing memory
static spinlock_t lock;

PCNode _allocate_new_cache_node(void)
{
    unsigned long page = get_zeroed_page(GFP_KERNEL);
    if (page) {
        PCNode node = (PCNode) page;
        INIT_LIST_HEAD(&node->sub_list);
        node->page = P2L(page);

        const int cache_node_size = sizeof(CacheNode);
        const int region_node_size = sizeof(AllocatedRegion);
        PARegion start = (PARegion) (page + cache_node_size);
        start->allocated_size = region_node_size;
        start->usage_size = 0;
        start->start_addr = P2L(start);
        list_add_tail(&start->node, &node->sub_list);

        PARegion end = (PARegion) (page + PAGE_SIZE - cache_node_size);
        end->allocated_size = region_node_size;
        end->usage_size = 0;
        end->start_addr = P2L(end);
        list_add_tail(&end->node, &node->sub_list);

        D(TAG, "Created a new page node %lu, start addr: %lu, end addr: %lu",
                page, P2L(start), P2L(end));
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
    D(TAG, "Try to find an available region for %d bytes in page: %lu, from %lu", 
            size, page->page, last->start_addr);

    PListHead ptr;
    PARegion region;
    void * result = NULL;

    list_for_each(ptr, &page->sub_list) {
        region = list_entry(ptr, AllocatedRegion, node);

        D(TAG, "Current region start addr: %lu", region->start_addr);
        unsigned long last_end_addr = last->start_addr + last->allocated_size;
        // be very carefull, because the type is unsigned long, 
        // which means the generated result won't be negative value
        if (region->start_addr >= last_end_addr && 
                region->start_addr - last_end_addr >= size + sizeof(AllocatedRegion)) {
            // find an region which has enough space to hold expected memory and an AllocatedRegion
            D(TAG, "Found a new region, last region addr: %lu, last region size: %d", 
                    last->start_addr, last->allocated_size);
            PARegion tmp = (PARegion) (last->start_addr + last->allocated_size);
            tmp->allocated_size = size + sizeof(AllocatedRegion);
            D(TAG, "New region addr: %lu, size: %d", P2L(tmp), tmp->allocated_size);
            tmp->usage_size = size;
            tmp->start_addr = P2L(tmp);

            // insert the new region behind the last node, make sure the order is correct
            D(TAG, "Insert new node %lu behind last node %lu, new region addr: %lu", 
                    P2L(&tmp->node), P2L(&last->node), P2L(tmp));
            list_add(&tmp->node, &last->node);

            result = (void *) tmp->start_addr + sizeof(AllocatedRegion);

            break;
        }
        last = region;
    }

    DEBUG_BLOCK(
        D(TAG, "Current nodes in page: %lu", page->page);
        int index = 0;
        list_for_each(ptr, &page->sub_list) {
            region = list_entry(ptr, AllocatedRegion, node);
            D(TAG, "Node #%d, start addr: %lu, size: %d", index, 
                     region->start_addr, region->allocated_size);
            index ++;
            if (index >= 5) break;
        }
    );
    return result;
}

void * alloc_mem(int size)
{
#ifdef DEBUG_M
   return kmalloc(size, GFP_KERNEL);
#else
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
        if (NULL != result) {
            goto release;
        }
    }
    D(TAG, "Didn't find an available region, try to create a new page_node");
    // no available region for new allocation, create a new page
    PCNode page_node = _allocate_new_cache_node();
    if (NULL != page_node) {
        list_add_tail(&page_node->node, &cache_nodes);

        result = _find_available_region_in_page(page_node, size);
    }
release:
    spin_unlock_wrapper(&lock);
    
    return result;
#endif
}

void _release_region_in_page(PCNode page, void *mem)
{
    PListHead ptr;
    PARegion curr;

    unsigned long addr = (unsigned long) mem;

    list_for_each(ptr, &page->sub_list) {
        curr = list_entry(ptr, AllocatedRegion, node);
        
        if (addr - sizeof(AllocatedRegion) == curr->start_addr) {
            // found the corresponding AllocatedRegion, delete the node from list first
            list_del(&curr->node);

            // clean up this chunk of memory
            memset((void *)curr->start_addr, 0, curr->allocated_size);
            break;
        }
    }
}

void release_mem(void * mem)
{
    if (NULL == mem) return;
#ifdef DEBUG_M
    kfree(mem);
#else
    PListHead ptr;
    PCNode curr;

    unsigned long addr = (unsigned long) mem;

    spin_lock_wrapper(&lock);

    list_for_each(ptr, &cache_nodes) {
        curr = list_entry(ptr, CacheNode, node);

        if (addr > curr->page && addr < curr->page + PAGE_SIZE) {
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

    spin_unlock_wrapper(&lock);
#endif
}

void release_mem_cache(void)
{
    while (!list_empty(&cache_nodes)) {
        PCNode tmp_node = list_first_entry_or_null(&cache_nodes, CacheNode, node);

        if (NULL != tmp_node) {
            list_del(&tmp_node->node);
            free_page((unsigned long) tmp_node->page);
        }
    }
}

