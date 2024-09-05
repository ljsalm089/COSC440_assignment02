# include <linux/slab.h> // for `kmalloc`
# include <linux/gfp.h>  // for `get_zeroed_page`
# include <linux/string.h> // for operations of string 


# include "common.h"
# include "page_buffer.h"

#define TAG "PageBuffer"

#define CONVERT(f, l) _PPBuffer f = _convert_pbuffer((l));

#define NODE_SIZE(n) ((n)->end_pos - (n)->start_pos)
#define NODE_AVAILABLE_SIZE(n) (PAGE_SIZE - (n)->end_pos)
#define NODE_START_POS(n) ((n)->page + (n)->start_pos)
#define NODE_END_POS(n) ((n)->page + (n)->end_pos)
#define NODE_IS_FULL(n) ((n)->end_pos == PAGE_SIZE)

typedef struct {
    void * page;
    size_t start_pos;
    size_t end_pos;
    ListHead node;
} PageNode;
typedef PageNode * PPageNode;

typedef struct {
    PBuffer inner;
    ListHead pages; 
} _PBuffer;

typedef _PBuffer * _PPBuffer;

static _PPBuffer _convert_pbuffer(PPBuffer b)
{
    _PPBuffer b = (_PPBuffer)((char *) b - offsetof(_PBuffer, inner));
    return b;
}

static void _release_page_node(PPageNode n)
{
    if (n) {
        if (n->page) free_page((unsigned long) n->page);

        kfree(n);
    }
}

static void _create_new_page_node()
{
    PPageNode node = (PPageNode) kmalloc(sizeof(PageNode), GFP_KERNEL);
    if (IS_ERR(node)) {
        int err = PTR_ERR(node);
        E(TAG, "Unable to allocate memory for PageNode: %d", err);
        return NULL;
    }
    memset(node, 0, sizeof(PageNode));

    node->page = (void *) get_zeroed_page(GFP_KERNEL);
    if (IS_ERR(node->page)) {
        int err = PTR_ERR(node->page);
        E(TAG, "Unable to alocate new page for buffer %d", err);

        kfree(node);
        return NULL;
    }

    return node;
}

static PPBuffer create_new_pbuffer()
{
    _PPBuffer p = (_PPBuffer) kmalloc (sizeof(_PBuffer), GFP_KERNEL);
    if (IS_ERR(p)) {
        int err = PTR_ERR(p);
        E(TAG, "Unable to allocate memory for PBuffer: %d", err);
        return NULL;
    }
    memset(p, 0, sizeof(_PBuffer));

    INIT_LIST_HEAD(&p->pages);
    return &p->inner;
}

static size_t pbuffer_size(PBuffer p) 
{
    CONVERT(buff, p);

    size_t size = 0;

    if (!list_empty(&buff->pages)) {
        PListHead ptr;
        PPageNode curr;

        list_for_each(ptr, &buff->pages) {
            curr = list_entry(ptr, PageNode, node);
            size += NODE_SIZE(curr);
        }
    }

    return size;
}

static size_t _write_into_page_node(PPageNode node, char * buff, size_t expected_size)
{
    int write_size = MIN(expected_size, NODE_AVAILABLE_SIZE(node));
    memcpy(NODE_END_POS(node), buff, write_size);
    node->end_pos += write_size;
    return write_size;
}

static size_t write_into_pbuffer(PPBuffer p, char * buff, size_t size)
{
    CONVERT(pb, p);

    size_t already_write_size = 0;

    while (already_write_size < size) {
        PPageNode node = NULL;
        if (list_empty(&pb->pages) || NODE_IS_FULL(list_last_entry(&pb->pages, 
                        PageNode, node))) {
            node = _create_new_page_node();
            if (!node) {
                E(TAG, "Unable to create new node");
                break;
            }
            list_add_tail(&node->node, &pb->pages);
        } else {
            node = list_last_entry(&pb->pages, PageNode, node);
        }
 
        already_write_size += _write_into_page_node(node, 
                buff + already_write_size, size - already_write_size);
    }

    return already_write_size;
}

static size_t _read_from_page_node(PPageNode node, char * buff, size_t expected_size)
{
    int read_size = MIN(expected_size, NODE_SIZE(node));
    memcpy(buff, NODE_START_POS(node), read_size);
    node->start_pos += read_size;
    return read_size;
}

static size_t read_from_pbuffer(PPBuffer p, char * buff, size_t size)
{
    CONVERT(pb, p);

    size_t already_read_size = 0;
    
    while (already_read_size < size) {
        PPageNode node = NULL;

        if (list_empty(&pb->pages) || 0 == NODE_SIZE(list_first_entry(&pb->pages, 
                        PageNode, node))) {
            break;
        } else {
            node = list_first_entry(&pb->pages, PageNode, node);
        }
        
        already_read_size += _read_from_page_node(node, buff + already_read_size,
                size - already_read_size);

        if (NODE_SIZE(node) == 0 && NODE_IS_FULL(node)) {
            list_del(&node->node);
        }
    }

    return already_read_size;
}

static size_t _simple_char_index(void * buff, size_t size, void *arg)
{
    char target = * ((char *) arg);
    char * result = strnchar(buff, size, target); 
    if (!result) {
        return -1;
    }
    return result - buff;
}

static size_t find_in_pbuffer_in_range(PPBuffer p, size_t end, 
        size_t (*index) (void *, size_t, void *), void *args)
{
    size_t target_pos = -1;

    if (!index) {
        index = _simple_char_index;
    }

    size_t node_first_pos = 0;
    CONVERT(pb, p);

    PListHead ptr;
    PPageNode curr;

    list_for_each(ptr, &pb->pages) {
        if (node_first_pos >= end) break;

        curr = list_entry(ptr, PageNode, node);

        void * start_in_node = NODE_START_POS(curr);
        size_t range = MIN(NODE_SIZE(curr), end - node_first_pos);
        size_t index_in_node = index(start_in_node, range, args);

        if (index_in_node >= 0) {
            target_pos = index_in_node + node_first_pos;
            break;
        }

        node_first_pos += NODE_SIZE(curr);
    }
    return target_pos;
}

static size_t find_in_pbuffer(PPBuffer p, size_t start_pos, 
        size_t (*index) (void *, size_t, void *), void *args)
{
    size_t target_pos = -1;

    if (!index) {
        index = _simple_char_index;
    }
    
    size_t node_first_pos = 0;

    CONVERT(pb, p);

    PListHead ptr;
    PPageNode cur;

    list_for_each(ptr, &pb->pages) {
        cur = list_entry(ptr, PageNode, node);

        void * start_in_node = NODE_START_POS(cur);
        size_t node_size = NODE_SIZE(cur);
        size_t index_in_node = -1;

        if (node_first_pos >= start_pos) {
            index_in_node = index(start_in_node, node_size, args);
            if (index_in_node >= 0) {
                target_pos = node_first_pos + index_in_node;
                break;
            }
        } else if (node_first_pos + node_size > start_pos) {
            int start_pos_in_node = start_pos - node_first_pos;
            index_in_node = index(start_in_node + start_pos_in_node, 
                    node_size - start_pos_in_node, args);
            if (index_in_node >= 0) {
                target_pos = start_pos + index_in_node;
                break;
            }
        }

        node_first_pos += node_size;
    }

    return target_pos;
}

static void release_pbuffer(PPBuffer p)
{
    while (!list_empty(&p->pages)) {
        PPageNode node = list_first_entry_or_null(&p->pages, PageNode, node);
        if (node) {
            list_del(&node->node);
            _release_page_node(node);
        }
    }

    kfree(p);
}
