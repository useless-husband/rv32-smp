/* Multicore support for bare-metal programs: hart identity, fork/join, and
 * the lock and counter primitives the benchmarks compare.  Every primitive
 * carries the RVWMO ordering it needs (aq/rl bits or FENCE), even though
 * this hardware is sequentially consistent: the code is meant to be correct
 * on any RISC-V multicore, not only on this one. */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>

#include "rv_platform.h"

#define RT_MAX_HARTS 8
#define RT_LINE 16          /* L1 line size in bytes */

static inline int rt_hartid(void)
{
    int h;
    __asm__ volatile("csrr %0, mhartid" : "=r"(h));
    return h;
}
static inline int rt_nharts(void) { return (int)*(volatile uint32_t *)RV_MMIO_NHARTS; }
static inline void rt_fence(void) { __asm__ volatile("fence rw, rw" ::: "memory"); }

/* Run fn(hart, arg) on harts 0..n-1 (n <= rt_nharts(); hart 0 is the caller)
 * and wait until every one has returned. */
typedef void (*rt_fn)(int hart, void *arg);
void rt_run_on(int n, rt_fn fn, void *arg);
static inline void rt_run_all(rt_fn fn, void *arg) { rt_run_on(rt_nharts(), fn, arg); }

/* A barrier for the harts of one rt_run_on() (sense reversing, AMO based). */
typedef struct { volatile uint32_t count; volatile uint32_t sense; } rt_barrier;
void rt_barrier_wait(rt_barrier *b, int n, uint32_t *local_sense);

/* -------------------------------------------------------------- atomics */
static inline uint32_t amo_add(volatile uint32_t *p, uint32_t v)
{
    uint32_t old;
    __asm__ volatile("amoadd.w.aqrl %0, %2, (%1)" : "=r"(old) : "r"(p), "r"(v) : "memory");
    return old;
}
static inline uint32_t amo_swap_acq(volatile uint32_t *p, uint32_t v)
{
    uint32_t old;
    __asm__ volatile("amoswap.w.aq %0, %2, (%1)" : "=r"(old) : "r"(p), "r"(v) : "memory");
    return old;
}
static inline uint32_t lr_w(volatile uint32_t *p)
{
    uint32_t v;
    __asm__ volatile("lr.w.aq %0, (%1)" : "=r"(v) : "r"(p) : "memory");
    return v;
}
/* returns 0 on success */
static inline uint32_t sc_w(volatile uint32_t *p, uint32_t v)
{
    uint32_t r;
    __asm__ volatile("sc.w.rl %0, %2, (%1)" : "=r"(r) : "r"(p), "r"(v) : "memory");
    return r;
}
/* fetch-and-add built from LR/SC (a constrained LR/SC loop) */
static inline uint32_t lrsc_add(volatile uint32_t *p, uint32_t v)
{
    uint32_t old, tmp, fail;
    __asm__ volatile("1: lr.w.aqrl %0, (%3)\n"
                     "   add %1, %0, %4\n"
                     "   sc.w.rl %2, %1, (%3)\n"
                     "   bnez %2, 1b"
                     : "=&r"(old), "=&r"(tmp), "=&r"(fail)
                     : "r"(p), "r"(v)
                     : "memory");
    return old;
}
static inline void store_release(volatile uint32_t *p, uint32_t v)
{
    __asm__ volatile("fence rw, w" ::: "memory");
    *p = v;
}
static inline uint32_t load_acquire(volatile uint32_t *p)
{
    uint32_t v = *p;
    __asm__ volatile("fence r, rw" ::: "memory");
    return v;
}

/* ---------------------------------------------------------------- locks */
/* test-and-test-and-set spinlock, the set done with AMOSWAP */
static inline void spin_lock_amo(volatile uint32_t *l)
{
    for (;;) {
        while (*l) { }
        if (amo_swap_acq(l, 1) == 0) return;
    }
}
/* the same lock, the set done with LR/SC */
static inline void spin_lock_lrsc(volatile uint32_t *l)
{
    uint32_t v, fail;
    __asm__ volatile("1: lw %0, (%2)\n"
                     "   bnez %0, 1b\n"
                     "   lr.w.aq %0, (%2)\n"
                     "   bnez %0, 1b\n"
                     "   sc.w %1, %3, (%2)\n"
                     "   bnez %1, 1b"
                     : "=&r"(v), "=&r"(fail)
                     : "r"(l), "r"(1u)
                     : "memory");
}
static inline void spin_unlock(volatile uint32_t *l) { store_release(l, 0); }

/* ticket lock: next ticket by fetch-and-add (AMO or LR/SC) */
typedef struct { volatile uint32_t next; uint32_t pad[3]; volatile uint32_t serving; } ticket_lock;
static inline void ticket_lock_amo(ticket_lock *t)
{
    uint32_t me = amo_add(&t->next, 1);
    while (load_acquire(&t->serving) != me) { }
}
static inline void ticket_lock_lrsc(ticket_lock *t)
{
    uint32_t me = lrsc_add(&t->next, 1);
    while (load_acquire(&t->serving) != me) { }
}
static inline void ticket_unlock(ticket_lock *t) { store_release(&t->serving, t->serving + 1); }

#endif
