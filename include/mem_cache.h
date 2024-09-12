#ifndef __MEM_CACHE_H__
#define __MEM_CACHE_H__

int init_mem_cache(void);

void * alloc_mem(int size, int in_interrupt);

void release_mem(void * mem, int in_interupt);

void release_mem_cache(void);

#endif  // __MEM_CACHE_H__
