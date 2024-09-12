# include <linux/gfp.h>  // for `get_zeroed_page`

# include "common.h"
# include "mem_cache.h"


#define TAG "MCache"


typedef struct {
    void * page;
    
    ListHead sub_list;

    ListHead pages;
} CacheNode;

typedef CacheNode * PCNode;

typedef struct {
} AllocatedRegion;

typedef AllocatedRegion * PARegion;


static ListHead cache_nodes;

int init_mem_cache()
{
    INIT_LIST_HEAD(&cache_nodes);
    return SUCC;
}

void * alloc_mem(int size);

void release_mem(void * mem);

void release_mem_cache()
{
    while (!list_empty(&cache_nodes)) {
        
    }
}

