/* Cross-core self-modifying code: hart 0 writes a small function into a RAM
 * buffer and does fence.i; hart 1 then does fence.i and calls it.  Hart 1's
 * instruction fetch misses, issues an IFetch on the bus, which snoops hart 0's
 * data cache (where the just-written, still-dirty instructions live) and gets
 * them.  This exercises the claim that fence.i works across harts. */
#include "rt.h"
#include "smp.h"

/* 8 bytes, 4-byte aligned, in RAM (.bss): "li a0, imm; ret" */
static volatile uint32_t code[4] __attribute__((aligned(16)));
static volatile uint32_t ready;
static int results_ok = 1;

static void writer(void)
{
    /* addi a0, x0, 0x123 ; jalr x0, 0(ra)  -- return 0x123 */
    code[0] = 0x12300513u;          /* addi a0, zero, 0x123 */
    code[1] = 0x00008067u;          /* ret */
    __asm__ volatile("fence.i" ::: "memory");
    store_release(&ready, 1);
}

static int reader(void)
{
    while (!load_acquire(&ready)) { }
    __asm__ volatile("fence.i" ::: "memory");
    int (*fn)(void) = (int (*)(void))(void *)code;
    return fn();
}

static void work(int hart, void *arg)
{
    (void)arg;
    if (hart == 0) writer();
    else if (hart == 1) {
        int r = reader();
        if (r != 0x123) results_ok = 0;
        printf("hart 1 called hart 0's freshly written code, got 0x%x (want 0x123)\n", (unsigned)r);
    }
}

int main(void)
{
    if (rt_nharts() < 2) {
        printf("smc: needs 2 harts, skipping\n");
        return 0;
    }
    rt_run_on(2, work, 0);
    printf(results_ok ? "smc: ok\n" : "smc: FAIL\n");
    return results_ok ? 0 : 1;
}
