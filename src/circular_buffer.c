# include <linux/slab.h> // For `kmalloc`
# include <linux/string.h>
# include <linux/stddef.h>

# include "circular_buffer.h"

# define TAG "CBuffer"

# define EXTRA_SIZE (sizeof(size_t) * 3 + sizeof(CBuffer))

# define CONVERT(b, c) _PCBuffer b = _convert_cbuffer((c))
# define W_POS(b) (b)->w_pos % (b)->total_size
# define R_POS(b) (b)->r_pos % (b)->total_size

typedef struct {
    size_t total_size;
    CBuffer inner;
    size_t r_pos;
    size_t w_pos;
    char buffer[];
} _CBuffer;

typedef _CBuffer * _PCBuffer;

static inline _PCBuffer _convert_cbuffer (PCBuffer buff)
{
    _PCBuffer b = (_PCBuffer) ((char *) buff - offsetof(_CBuffer, inner));
    return b;
}

static PCBuffer create_new_cbuffer (size_t size) 
{
    size_t allocated_size = size + EXTRA_SIZE;
    _PCBuffer buff = (_PCBuffer) kmalloc(allocated_size, GFP_KERNEL);
    if (IS_ERR(buff)) {
        int err = PTR_ERR(buff);
        E(TAG, "Unable to allocate memory for CBuffer: %d", err);
        return NULL;
    }
    return init_new_cbuffer(buff, allocated_size);
}

static PCBuffer init_new_cbuffer(void * p, size_t mem_size) 
{
    size_t size = mem_size - EXTRA_SIZE;
    if (size <= 0) {
        W(TAG, "No enough memory to create CBuffer");
        return NULL;
    }
    memset(p, 0, mem_size);
    _PCBuffer buff = (_PCBuffer) p;
    buff->total_size = size;
    return &buff->inner;
}

static void release_cbuffer(PCBuffer cbuff)
{
    if (cbuff) {
        CONVERT(buff, cbuff);
        kfree(buff);
    }
}

static size_t _buff_size (_PCBuffer buff) 
{
    return buff->w_pos - buff->r_pos;
}

static void _adjust_pos(_PCBuffer buff)
{
    size_t total_size = buff->total_size;
    while (buff->w_pos > total_size && buff->r_pos > total_size) {
        buff->w_pos -= total_size;
        buff->r_pos -= total_size;
    }
}

static size_t cbuffer_size(PCBuffer cbuff) 
{
    CONVERT(buff, cbuff);
    return _buff_size(buff);
}

static size_t cbuffer_available_size(PCBuffer cbuff) 
{
    CONVERT(buff, cbuff);
    return buff->total_size - cbuffer_size(cbuff);
}

size_t write_into_cbuffer(PCBuffer cbuff, char * buff, size_t size)
{
    size_t available_size = cbuffer_available_size(cbuff);
    size_t target_write_size = size;
    if (size > available_size) {
        target_write_size = available_size;
    }
    size_t already_write_size = 0;
    
    CONVERT(pb, cbuff);

    while (W_POS(pb) + (target_write_size - already_write_size) > pb->total_size) {
        size_t write_size = MIN(target_write_size - already_write_size, 
                pb->total_size - W_POS(pb));
        memcpy(pb->buffer + W_POS(pb), buff + already_write_size, write_size);
        pb->w_pos += write_size;
        already_write_size += write_size;
    }

    _adjust_pos(pb);
    return already_write_size;
}

size_t read_from_cbuffer(PCBuffer cbuff, char * buff, size_t size)
{
    size_t buffer_size = cbuffer_size(cbuff);
    size_t target_read_size = size;
    if (size > buffer_size) {
        target_read_size = buffer_size;
    }

    size_t already_read_size = 0;

    CONVERT(pb, cbuff);

    while (R_POS(pb) + (target_read_size - already_read_size) > pb->total_size) {
        size_t read_size = MIN(target_read_size - already_read_size, 
                pb->total_size - R_POS(pb));
        memcpy(buff + already_read_size, pb->buffer + R_POS(pb), read_size);
        pb->r_pos += read_size;
        already_read_size += read_size;
    }

    _adjust_pos(pb);
    return already_read_size;
}
