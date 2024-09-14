# include <linux/kernel.h>
# include <linux/types.h>
# include <linux/string.h>
# include <linux/spinlock.h>

# include "common.h"
# include "mem_cache.h"
# include "page_buffer.h"
# include "delimiter_buffer.h"

# define TAG "DelimiterBuff"

# define CONVERT(p, b) _PDBuffer p = _convert((b))

typedef struct {
    unsigned short has_delimiter;
    size_t buffer_size;

    ListHead node;
} DRecord;

typedef DRecord * PDRecord;


typedef struct {
    DelimiterBuffer inner;
    PPBuffer page_buffer;

    // there should be at least one record in this list
    ListHead records;

    spinlock_t lock;
} _DBuffer;

typedef _DBuffer * _PDBuffer;


inline _PDBuffer _convert(PDBuffer b) 
{
    return (_PDBuffer) ((char *) b - offsetof(_DBuffer, inner));
}

PDBuffer create_new_dbuffer(void)
{
    _PDBuffer p = (_PDBuffer) alloc_mem(sizeof(_DBuffer));
    if (!p) {
        return NULL;
    }

    p->page_buffer = create_new_pbuffer(); 
    if (!p->page_buffer) goto error_with_pdbuffer;

    INIT_LIST_HEAD(&p->records);

    PDRecord record = (PDRecord) alloc_mem(sizeof(DRecord));
    if (!record) goto error_with_page_buffer;
    memset(record, 0, sizeof(DRecord));

    // at least add a record to the list
    list_add_tail(&record->node, &p->records);
    return &p->inner;

error_with_page_buffer:
    release_pbuffer(p->page_buffer);

error_with_pdbuffer:
    release_mem(p);
    return NULL;
}

void release_dbuffer(PDBuffer buff)
{
    if (!buff) return;

    CONVERT(pb, buff);
    
    while (!list_empty(&pb->records)) {
        PDRecord first = list_first_entry_or_null(&pb->records, DRecord, node);
        if (first) {
            list_del(&first->node);
            release_mem(first);
        }
    }

    release_pbuffer(pb->page_buffer);

    release_mem(pb);
}

size_t write_into_dbuffer(PDBuffer b, void * buff, size_t size)
{
    CONVERT(pb, b);

    const char delimiter = '\0';

    spin_lock_wrapper(&pb->lock);

    PDRecord last_record = list_last_entry(&pb->records, DRecord, node);
    size_t write_size = write_into_pbuffer(pb->page_buffer, buff, size);
    D(TAG, "Successfully write %d bytes data into dbuffer from %lu", 
            write_size, P2L(buff));

    if (write_size > 0) {
        // detect all demiliters and generate corresponding delimiter records
        // update the buffer size before the delimiter
        void * check_buff = buff;
        int index = simple_char_index(check_buff, write_size, (void *) &delimiter);
        if (index >= 0) {
            while (index >= 0) {
                D(TAG, "Found delimiter in the buffer, position is: %d", index);
                D(TAG, "String before delimiter: %s", (char *)check_buff);

                last_record->buffer_size += index;
                last_record->has_delimiter = 1;
    
                // generate a new record and append it to the list
                last_record = alloc_mem(sizeof(DRecord));
                if (NULL == last_record) {
                    break;
                }
                memset(last_record, 0, sizeof(DRecord));
                list_add_tail(&last_record->node, &pb->records);

                check_buff += index + 1;
                if (check_buff >= buff + write_size) {
                    break;
                }
                // make sure all delimiters in the buffer have been recorded
                index = simple_char_index(check_buff, buff + write_size - check_buff, 
                        (void *) &delimiter);
            }
        } else {
            last_record->buffer_size += write_size;
        }
    }
    spin_unlock_wrapper(&pb->lock);
    return write_size;
}

size_t _read_from_dbuffer_generic(PDBuffer pb, void * buff, size_t size, int to_user)
{
    CONVERT(b, pb);
    spin_lock_wrapper(&b->lock);

    size_t read_size = 0;
    PDRecord record = list_first_entry(&b->records, DRecord, node);
    if (record->buffer_size > 0) {
        // make sure the part exceeds the delimiter is not read
        size = MIN(record->buffer_size, size);

        read_size = to_user ? read_from_pbuffer_into_user(b->page_buffer, buff, size) 
            : read_from_pbuffer(b->page_buffer, buff, size);
        if (read_size >= 0) {
            record->buffer_size -= read_size;
        }
    }

    spin_unlock_wrapper(&b->lock);
    return read_size;
}

size_t read_from_dbuffer(PDBuffer pb, void * buff, size_t size)
{
    return _read_from_dbuffer_generic(pb, buff, size, 0);
}

size_t read_from_dbuffer_to_user(PDBuffer pb, void __user * buff, size_t size)
{
    return _read_from_dbuffer_generic(pb, buff, size, 1);
}

/**
 * check if the dbuffer contains data to read
 * @return: -1 means no more data to read; 
 *          0 means need to wait; 
 *          positive value means how many bytes of data in the buffer
 */
int dbuffer_contains_data(PDBuffer pb)
{
    CONVERT(buff, pb);

    int result = 0;

    spin_lock_wrapper(&buff->lock);

    PDRecord record = list_first_entry(&buff->records, DRecord, node);
    if (record->has_delimiter) {
        // has recognised the delimiter, only the data in the buffer can be read
        result = record->buffer_size ? record->buffer_size : -1;
    } else {
        // hasn't recognised the delimiter yet, 
        // probably there is some data event though no data in buffer
        result = record->buffer_size;
    }

    spin_unlock_wrapper(&buff->lock);

    return result;
}

void dbuffer_end_phase_reading(PDBuffer buff)
{
    CONVERT(pb, buff);

    spin_lock_wrapper(&pb->lock);

    // only function to remove the records from the list (except the release function)
    PDRecord record = list_first_entry(&pb->records,  DRecord, node);
    if (record->has_delimiter && 0 == record->buffer_size) {
        // all the data before the delimiter has been read,
        // remove the delimiter and current record
        list_del(&record->node);
        if (list_empty(&pb->records)) {
            // no more records in the list, reuse the old one, 
            // make sure that at least one record is in the list
            memset(record, 0, sizeof(DRecord));
            list_add_tail(&record->node, &pb->records);
        }
        // read the delimiter out from the buffer
        char tmp;
        read_from_pbuffer(pb->page_buffer, &tmp, sizeof(char));
    } else {
        // hasn't recognised the delimiter or there is still some data in the buffer
        // do nothing, keep the data and record for next turn of reading
    }

    spin_unlock_wrapper(&pb->lock);
}
