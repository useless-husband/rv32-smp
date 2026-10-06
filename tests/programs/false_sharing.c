/* False sharing and its fix.  Each hart increments its own counter a million
 * times with a plain (non-atomic) load/store - no sharing of data, only of
 * cache lines.  In the "packed" layout the counters are adjacent words of one
 * line, so every increment on one hart invalidates the others' line (BusRdX
 * ping-pong); in the "padded" layout each counter sits on its own line and
 * the harts never contend.  The program prints the cycles and the bus
 * invalidation count of each; the fix should be far faster with far fewer
 * invalidations. */
#include "rt.h"
#include "smp.h"

#define ITER 20000

typedef struct { volatile uint32_t v; } packed_t;
typedef struct { volatile uint32_t v; uint32_t pad[RT_LINE / 4 - 1]; } padded_t;

static packed_t packed[RT_MAX_HARTS];
static padded_t padded[RT_MAX_HARTS];
static int use_padded;

static void work(int hart, void *arg)
{
    (void)arg;
    if (use_padded)
        for (int i = 0; i < ITER; i++) padded[hart].v = padded[hart].v + 1;
    else
        for (int i = 0; i < ITER; i++) packed[hart].v = packed[hart].v + 1;
}

static uint32_t run(int padded_layout)
{
    use_padded = padded_layout;
    rt_fence();
    uint64_t c0 = rdcycle64();
    rt_run_all(work, 0);
    return (uint32_t)(rdcycle64() - c0);
}

int main(void)
{
    int n = rt_nharts();
    uint32_t cp = run(0);
    uint32_t cq = run(1);
    printf("false sharing, %d harts, %d increments each:\n", n, ITER);
    printf("  packed (one line)   : %u cycles\n", (unsigned)cp);
    printf("  padded (per line)   : %u cycles\n", (unsigned)cq);
    if (cq) printf("  speed-up from the fix: %u.%02ux\n", cp / cq, (cp % cq) * 100 / cq);
    return 0;
}
