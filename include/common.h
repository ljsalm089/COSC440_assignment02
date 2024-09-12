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

#endif // DEBUG
       
#define currentpid current->pid

#define MIN(l, r) (l) < (r) ? (l) : (r)
#define MAX(l, r) (l) > (r) ? (l) : (r)

#define SUCC 0
#define FAIL -1

# include <linux/list.h>

typedef struct list_head ListHead;
typedef ListHead * PListHead;


#endif // __COMMON_H__
