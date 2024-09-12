#ifndef __MEM_CACHE_H__
#define __MEM_CACHE_H__

int init_mem_cache();

void * alloc_mem(int size);

void release_mem(void * mem);

void release_mem_cache();

#endif  // __MEM_CACHE_H__
