#ifndef __COMMON_H__

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <error.h>

#ifndef NULL
#define NULL ((void *)0)
#endif // NULL


#define LOG(...) printf(__VA_ARGS__)

#define __DEBUG__ 1
#ifdef __DEBUG__

#include <time.h>

time_t now;
struct tm* c;

#define TIME() time(&now) && (c = localtime(&now)) \
                    && printf("%d-%02d-%02d %02d:%02d:%02d  ", c->tm_year + 1900, c->tm_mon + 1, c->tm_mday, \
                    c->tm_hour, c->tm_min, c->tm_sec)

#define LOG_PREFIX(l, t) TIME() && printf("%s  %s\t", l, t)

#define INFO(t_, ...) LOG_PREFIX("I", t_) && printf(__VA_ARGS__) && printf("\n")
#define DEBUG(t_, ...) LOG_PREFIX("D", t_) && printf(__VA_ARGS__) && printf("\n")
#define ERROR(t_, ...) LOG_PREFIX("E", t_) && printf(__VA_ARGS__) && printf("\n")

#else
#define INFO(t_, ...) 
#define DEBUG(t_, ...)
#define ERROR(t_, ...)

#endif // __DEBUG__

#define err_sys(t_, ...) ERROR((t_), __VA_ARGS__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1)


#endif // __COMMON_H__
