/* Minimal bare-metal runtime: console output, exit, a few string functions
 * and access to the performance counters. */
#ifndef RT_H
#define RT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);
int putchar(int c);
int puts(const char *s);
void exit(int code) __attribute__((noreturn));

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char *strcpy(char *d, const char *s);
int strcmp(const char *a, const char *b);

static inline uint64_t rdcycle64(void)
{
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("rdcycleh %0" : "=r"(hi));
        __asm__ volatile("rdcycle %0" : "=r"(lo));
        __asm__ volatile("rdcycleh %0" : "=r"(hi2));
    } while (hi != hi2);
    return (uint64_t)hi << 32 | lo;
}

static inline uint64_t rdinstret64(void)
{
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("rdinstreth %0" : "=r"(hi));
        __asm__ volatile("rdinstret %0" : "=r"(lo));
        __asm__ volatile("rdinstreth %0" : "=r"(hi2));
    } while (hi != hi2);
    return (uint64_t)hi << 32 | lo;
}

/* mhpmcounter3..12 (see model/rv_platform.h for the event of each) */
uint32_t rt_hpm(int n);

/* Print cycles, instructions, CPI and the counters since `rt_stats_begin`. */
void rt_stats_begin(void);
void rt_stats_end(const char *label);

#endif
