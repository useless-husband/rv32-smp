/* Bug museum 5: an LR lock-out that never ends.
 * Hart 0 does LR on X and then waits for a flag before its SC (a loop the
 * ISA gives no progress guarantee for - but it must not stop OTHER harts).
 * Hart 1 stores to X, then sets the flag.  The lock-out window delays hart
 * 1's store for at most LOCKOUT cycles; with BUG_LOCKOUT_FOREVER the store
 * waits for hart 0's SC, which waits for the flag, which waits for the
 * store: a deadlock that the bus cannot break. */
#include "museum.h"

static volatile uint32_t x __attribute__((aligned(16)));
static volatile uint32_t flag __attribute__((aligned(16)));
static volatile uint32_t sc_result;

static void work(int hart, void *arg)
{
    (void)arg;
    uint32_t sense = 0;
    sync2(&sense);
    if (hart == 0) {
        uint32_t v = lr_w(&x);
        while (!flag) { }
        sc_result = sc_w(&x, v + 1);
    } else {
        delay(4);
        x = 7;
        flag = 1;
    }
    sync2(&sense);
}

int main(void)
{
    rt_run_on(2, work, 0);
    printf("lockout: finished, SC %s\n", sc_result ? "failed (as it must)" : "succeeded");
    return sc_result == 0;
}
