/* Shared helpers for the bug-museum programs: two harts, a barrier, and a
 * delay that varies from round to round so the race window is hit at many
 * different relative timings. */
#ifndef MUSEUM_H
#define MUSEUM_H
#include "rt.h"
#include "smp.h"

static rt_barrier mb __attribute__((aligned(16)));

/* about two cycles per step, no memory access */
static inline void delay(int n)
{
    if (n > 0) __asm__ volatile("1: addi %0, %0, -1\n   bnez %0, 1b" : "+r"(n));
}
static inline void sync2(uint32_t *sense) { rt_barrier_wait(&mb, 2, sense); }
#endif
