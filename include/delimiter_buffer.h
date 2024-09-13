#ifndef __DELIMITER_BUFFER_H__
#define __DELIMITER_BUFFER_H__

typedef struct {
} DelimiterBuffer;

typedef DelimiterBuffer * PDBuffer;

PDBuffer create_new_dbuffer(void);

void release_dbuffer(PDBuffer buff);

size_t write_into_dbuffer(PDBuffer pb, void * buff, size_t size);

size_t read_from_dbuffer(PDBuffer pb, void * buff, size_t size);

size_t read_from_dbuffer_to_user(PDBuffer pb, void __user * buff, size_t size);

int dbuffer_contains_data(PDBuffer pb);

void dbuffer_end_phase_reading(PDBuffer pb);

#endif // __DELIMITER_BUFFER_H__
