/* Litmus tests from litmus-tests-riscv, run on the RTL many times with the
 * timing perturbed from outside (--seed, --stall-pct, --jitter) and from
 * within (a per-iteration, per-hart delay).  For each test the program
 * records how often the "exists" (non-sequentially-consistent) outcome is
 * observed; tests/system/test_litmus.py compares that against the reference
 * model's verdict in data/litmus/model-results/flat.logs (the operational
 * RVWMO model shipped with the suite).
 *
 * Every outcome this core produces must be allowed by the reference model.
 * The core performs every access in program order at one point, so it is
 * sequentially consistent: it never shows a relaxed outcome even where the
 * reference model (and so RVWMO) allows one.  The program reports that too.
 *
 * x, y, a, b live on separate cache lines so a reordering, if the core did
 * one, would be visible. */
#include "rt.h"
#include "smp.h"

#ifndef RUNS
#define RUNS 3000
#endif

static volatile uint32_t v[16] __attribute__((aligned(64)));   /* x=v[0] y=v[4] a=v[8] b=v[12] */
static volatile uint32_t r[RT_MAX_HARTS][4] __attribute__((aligned(64)));
static rt_barrier bar __attribute__((aligned(64)));
static volatile uint32_t relaxed[16], total[16];

static inline void delay(int n)
{
    if (n > 0) __asm__ volatile("1: addi %0, %0, -1\n   bnez %0, 1b" : "+r"(n));
}

static inline void jig(uint32_t *lcg, int hart)
{
    *lcg = *lcg * 1664525u + 1013904223u;
    delay((int)((*lcg >> 8) & 7) * (hart + 1));
}

/* each test body returns nothing; it reads/writes v[] and leaves regs in r[hart][] */
static void mp(int h, int fence)
{
    if (h == 0) {
        v[0] = 1;
        if (fence) rt_fence();
        v[4] = 1;
    } else if (h == 1) {
        uint32_t r1 = v[4];
        if (fence) rt_fence();
        uint32_t r2 = v[0];
        r[1][0] = r1; r[1][1] = r2;
    }
}
static int mp_relaxed(void) { return r[1][0] == 1 && r[1][1] == 0; }

static void sb(int h, int fence)
{
    if (h < 2) {
        int o = h == 0 ? 0 : 4, p = h == 0 ? 4 : 0;
        v[o] = 1;
        if (fence) rt_fence();
        r[h][0] = v[p];
    }
}
static void sb_amo(int h)   /* AMO-FENCE: amoswap.aqrl stands in for the fence */
{
    if (h < 2) {
        int o = h == 0 ? 0 : 4, p = h == 0 ? 4 : 0, s = h == 0 ? 8 : 12;
        v[o] = 1;
        (void)amo_swap_acq(&v[s], 1);   /* aq here; aqrl in the inline below */
        __asm__ volatile("amoswap.w.aqrl x0, %0, (%1)" :: "r"(1u), "r"(&v[s]) : "memory");
        r[h][0] = v[p];
    }
}
static int sb_relaxed(void) { return r[0][0] == 0 && r[1][0] == 0; }

static void lb(int h, int fence)
{
    if (h < 2) {
        int rd = h == 0 ? 0 : 4, wr = h == 0 ? 4 : 0;
        r[h][0] = v[rd];
        if (fence) rt_fence();
        v[wr] = 1;
    }
}
static int lb_relaxed(void) { return r[0][0] == 1 && r[1][0] == 1; }

static void twotwo(int h, int fence)   /* 2+2W */
{
    if (h < 2) {
        int a = h == 0 ? 0 : 4, b = h == 0 ? 4 : 0;
        v[a] = 2;
        if (fence) rt_fence();
        v[b] = 1;
    }
}
static int twotwo_relaxed(void) { return v[0] == 2 && v[4] == 2; }

static void iriw(int h, int fence)   /* 4 threads; writers 0,1; observers 2,3 */
{
    if (h == 0) v[0] = 1;
    else if (h == 1) v[4] = 1;
    else if (h == 2) {
        uint32_t a = v[0];
        if (fence) rt_fence();
        uint32_t b = v[4];
        r[2][0] = a; r[2][1] = b;
    } else if (h == 3) {
        uint32_t a = v[4];
        if (fence) rt_fence();
        uint32_t b = v[0];
        r[3][0] = a; r[3][1] = b;
    }
}
static int iriw_relaxed(void) { return r[2][0] == 1 && r[2][1] == 0 && r[3][0] == 1 && r[3][1] == 0; }

struct Test {
    const char *name;
    int nthreads, fence, slot;
    void (*body)(int, int);
    int (*relaxed)(void);
};

static void amo_body(int h, int f) { (void)f; sb_amo(h); }

static const struct Test tests[] = {
    {"MP", 2, 0, 0, mp, mp_relaxed},
    {"MP+fence.rw.rws", 2, 1, 1, mp, mp_relaxed},
    {"SB", 2, 0, 2, sb, sb_relaxed},
    {"SB+fence.rw.rws", 2, 1, 3, sb, sb_relaxed},
    {"LB", 2, 0, 4, lb, lb_relaxed},
    {"LB+fence.rw.rws", 2, 1, 5, lb, lb_relaxed},
    {"2+2W", 2, 0, 6, twotwo, twotwo_relaxed},
    {"2+2W+fence.rw.rws", 2, 1, 7, twotwo, twotwo_relaxed},
    {"IRIW+fence.rw.rws", 4, 1, 8, iriw, iriw_relaxed},
    {"AMO-FENCE", 2, 0, 9, amo_body, sb_relaxed},
};
#define NTESTS ((int)(sizeof tests / sizeof tests[0]))

static int g_nharts;

static void runner(int h, void *arg)
{
    (void)arg;
    uint32_t sense = 0, lcg = 0x1234567u + (uint32_t)h * 2246822519u;
    for (int ti = 0; ti < NTESTS; ti++) {
        const struct Test *t = &tests[ti];
        for (int it = 0; it < RUNS; it++) {
            if (h == 0) { v[0] = v[4] = v[8] = v[12] = 0; }
            rt_barrier_wait(&bar, g_nharts, &sense);
            if (h < t->nthreads) {
                jig(&lcg, h);
                t->body(h, t->fence);
            }
            rt_barrier_wait(&bar, g_nharts, &sense);
            if (h == 0) {
                total[t->slot]++;
                if (t->relaxed()) relaxed[t->slot]++;
            }
            rt_barrier_wait(&bar, g_nharts, &sense);
        }
    }
}

int main(void)
{
    g_nharts = rt_nharts();
    rt_run_all(runner, 0);
    printf("{\"harts\": %d, \"runs_per_test\": %d, \"tests\": [\n", g_nharts, RUNS);
    for (int ti = 0; ti < NTESTS; ti++) {
        const struct Test *t = &tests[ti];
        int ran = t->nthreads <= g_nharts;
        printf("  {\"name\": \"%s\", \"threads\": %d, \"ran\": %s, \"runs\": %u, \"relaxed\": %u}%s\n", t->name,
               t->nthreads, ran ? "true" : "false", (unsigned)total[t->slot], (unsigned)relaxed[t->slot],
               ti + 1 < NTESTS ? "," : "");
    }
    printf("]}\n");
    return 0;
}
