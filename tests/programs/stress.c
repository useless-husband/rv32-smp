/* Random multi-core stress.  Every hart runs a long, deterministic stream of
 * random memory operations (load, store, AMO, and LR/SC pairs) over a small
 * shared region that spans a handful of cache lines, so the caches are
 * forced through every coherence transition against each other.  Timing is
 * perturbed from outside (--seed, --stall-pct, --jitter), so one binary
 * covers many interleavings.
 *
 * Two invariants are checked end to end:
 *   * a shared counter updated only by amoadd ends at exactly the number of
 *     increments (no lost update);
 *   * every value any load reads is explained by a legal total order of all
 *     the accesses - checked outside, by the golden memory checker in the
 *     harness (sim/memcheck.h), on every run.
 * The base seed is a compile-time macro so a suite can build many streams. */
#include "rt.h"
#include "smp.h"

#ifndef SEED
#define SEED 1
#endif
#define ITER 4000
#define WORDS 24                 /* 6 lines of 4 words */

static volatile uint32_t shared[WORDS] __attribute__((aligned(64)));
static volatile uint32_t counter;
static volatile uint32_t adds;   /* total amoadd increments, itself by amoadd */
static rt_barrier bar __attribute__((aligned(16)));

static void work(int hart, void *arg)
{
    (void)arg;
    uint32_t sense = 0;
    rt_barrier_wait(&bar, rt_nharts(), &sense);
    uint32_t x = (uint32_t)(SEED * 2654435761u) ^ (uint32_t)(hart + 1) * 40503u;
    for (int i = 0; i < ITER; i++) {
        x = x * 1664525u + 1013904223u;
        uint32_t idx = (x >> 8) % WORDS;
        switch ((x >> 3) & 7) {
        case 0: case 1: (void)shared[idx]; break;                 /* load */
        case 2: shared[idx] = x; break;                           /* store */
        case 3: amo_add(&shared[idx], 1); break;
        case 4: (void)amo_swap_acq(&shared[idx], x); break;
        case 5: {                                                 /* LR/SC try */
            uint32_t v = lr_w(&shared[idx]);
            (void)sc_w(&shared[idx], v + 1);
            break;
        }
        case 6: amo_add(&counter, 1); amo_add(&adds, 1); break;   /* the counted invariant */
        default: (void)load_acquire(&shared[idx]); break;
        }
    }
    rt_barrier_wait(&bar, rt_nharts(), &sense);
}

int main(void)
{
    rt_run_all(work, 0);
    int ok = counter == adds;
    printf("stress seed %d: %d harts x %d ops, counter %u (expected %u): %s\n", (int)SEED, rt_nharts(), ITER,
           (unsigned)counter, (unsigned)adds, ok ? "ok" : "LOST UPDATE");
    return ok ? 0 : 1;
}
