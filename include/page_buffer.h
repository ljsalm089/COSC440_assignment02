#ifndef __PAGE_BUFFER_H__
#define __PAGE_BUFFER_H__

typedef struct {
} PBuffer;

typedef PBuffer * PPBuffer;

static PPBuffer create_new_pbuffer(void);

static size_t pbuffer_size(PBuffer p);

static size_t write_into_pbuffer(PPBuffer p, char * buff, size_t size);

static size_t read_from_pbuffer(PPBuffer p, char * buff, size_t size);

static size_t find_in_pbuffer_in_range(PPBuffer p, size_t end, 
        size_t (*index) (void *, size_t, void *), void *args);

static size_t find_in_pbuffer(PPBuffer p, size_t start_pos, 
        size_t (*index) (void *, size_t, void *), void *args);

static void release_pbuffer(PPBuffer p);

#endif // __PAGE_BUFFER_H__
