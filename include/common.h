#ifndef __COMMON_H__
#define __COMMON_H__

#include <linux/printk.h>

#define DEBUG
#ifdef DEBUG

#define D(t_, fmt, ...) printk(KERN_DEBUG "%s: " fmt, t_, ##__VA_ARGS__)
#define I(t_, fmt, ...) printk(KERN_INFO "%s: " fmt, t_, ##__VA_ARGS__)
#define W(t_, fmt, ...) printk(KERN_WARNING "%s: " fmt, t_, ##__VA_ARGS__)
#define A(t_, fmt, ...) printk(KERN_ALERT "%s: " fmt, t_, ##__VA_ARGS__)
#define E(t_, fmt, ...) printk(KERN_ERR "%s: " fmt, t_, ##__VA_ARGS__)

#define DEBUG_BLOCK(x) x

#else

#define D(T_, fmt, ...) 
#define I(t_, fmt, ...) printk(KERN_INFO fmt, ##__VA_ARGS__)
#define W(t_, fmt, ...) printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define A(t_, fmt, ...) printk(KERN_ALERT fmt, ##__VA_ARGS__)
#define E(t_, fmt, ...) printk(KERN_ERR fmt, ##__VA_ARGS__)

#define DEBUG_BLOCK(x)

#define spin_lock_wrapper(l) unsigned long __flags; \
    if (in_interrupt()) {\
        D(TAG, "Locking " #l); \
        spin_lock_irqsave((l), __flags);\
        D(TAG, "Locked " #l);\
    } else {\
        D(TAG, "Locking " #l); \
        spin_lock((l));\
        D(TAG, "Locked " #l);\
    }


#define spin_unlock_wrapper(l) if (in_interrupt()) {\
    D(TAG, "Unlocking " #l); \
    spin_unlock_irqrestore((l), __flags);\
    D(TAG, "Unlocked " #l);\
} else {\
    D(TAG, "Unlocking " #l); \
    spin_unlock((l));\
    D(TAG, "Unlocked " #l);\
}

#endif // DEBUG
       
#define currentpid current->pid

#define MIN(l, r) (l) < (r) ? (l) : (r)
#define MAX(l, r) (l) > (r) ? (l) : (r)

#define SUCC 0
#define FAIL -1

# include <linux/list.h>

typedef struct list_head ListHead;
typedef ListHead * PListHead;

#define P2L(r) ((unsigned long) (r))

#define spin_lock_wrapper(l) unsigned long __flags; \
    if (in_interrupt()) {\
        spin_lock_irqsave((l), __flags);\
    } else {\
        spin_lock((l));\
    }

#define spin_unlock_wrapper(l) if (in_interrupt()) {\
    spin_unlock_irqrestore((l), __flags);\
} else {\
    spin_unlock((l));\
}

#endif // __COMMON_H__
