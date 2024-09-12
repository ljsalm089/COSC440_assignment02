#ifndef __PAGE_BUFFER_H__
#define __PAGE_BUFFER_H__

typedef struct {
} PBuffer;

typedef PBuffer * PPBuffer;

PPBuffer create_new_pbuffer(void);

size_t pbuffer_size(PPBuffer p);

size_t write_into_pbuffer(PPBuffer p, char * buff, size_t size);

size_t read_from_pbuffer(PPBuffer p, char * buff, size_t size);

size_t get_from_pbuffer(PPBuffer p, char * buff, size_t size);

size_t write_into_pbuffer_from_user (PPBuffer p, char __user * buff, size_t size);

size_t read_from_pbuffer_into_user (PPBuffer p, char __user * buff, size_t size);

size_t find_in_pbuffer_in_range(PPBuffer p, size_t end, 
        size_t (*index) (void *, size_t, void *), void *args);

size_t find_in_pbuffer(PPBuffer p, size_t start_pos, 
        size_t (*index) (void *, size_t, void *), void *args);

size_t simple_char_index(void * buff, size_t size, void *arg);

void release_pbuffer(PPBuffer p);

#endif // __PAGE_BUFFER_H__
