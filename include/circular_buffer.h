#ifndef __cbuffer_H__
#define __cbuffer_H__

#define CBUFFER_SIZE 50

typedef struct {
} CBuffer;

typedef CBuffer* PCBuffer;

PCBuffer create_new_cbuffer (size_t size);

PCBuffer init_new_cbuffer(void * p, size_t mem_size);

void release_cbuffer(PCBuffer cbuff);

size_t cbuffer_size(PCBuffer cbuff);

size_t cbuffer_available_size(PCBuffer cbuff);

void release_cbuffer(PCBuffer cbuff);

size_t write_into_cbuffer(PCBuffer cbuff, char * buff, size_t size);

size_t read_from_cbuffer(PCBuffer cbuff, char * buff, size_t size);

#endif // __cbuffer_H__
