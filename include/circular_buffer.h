#ifndef __cbuffer_H__
#define __cbuffer_H__

# include "common.h"

#define CBUFFER_SIZE 50

typedef struct {
} CBuffer;

typedef CBuffer* PCBuffer;

static PCBuffer create_new_cbuffer (size_t size);

static PCBuffer init_new_cbuffer(void * p, size_t mem_size);

static void release_cbuffer(PCBuffer cbuff);

static size_t cbuffer_size(PCBuffer cbuff);

static size_t cbuffer_available_size(PCBuffer cbuff);

static void release_cbuffer(PCBuffer cbuff);

static size_t write_into_cbuffer(PCBuffer cbuff, char * buff, size_t size);

static size_t read_from_cbuffer(PCBuffer cbuff, char * buff, size_t size);

#endif // __cbuffer_H__
