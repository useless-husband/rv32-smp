/* Bug museum 3: an LR reservation that survives a remote store.
 * Hart 0 loads X with LR, works for a while (longer than the lock-out
 * window), then tries SC.  Meanwhile hart 1 stores to X.  The SC must fail;
 * with BUG_LRSC_NO_CLEAR the remote invalidation leaves the reservation in
 * place and the SC succeeds, overwriting hart 1's store - the atomicity an
 * LR/SC pair promises is gone. */
#include "museum.h"

#define ROUNDS 20
static volatile uint32_t x __attribute__((aligned(16)));
static volatile uint32_t bad;

static void work(int hart, void *arg)
{
    (void)arg;
    uint32_t sense = 0;
    for (uint32_t r = 1; r <= ROUNDS; r++) {
        sync2(&sense);
        if (hart == 0) {
            uint32_t v = lr_w(&x);
            delay(40);                 /* hart 1's store lands in here */
            uint32_t fail = sc_w(&x, v + 1);
            if (!fail) bad++;          /* x was written by hart 1 since the LR */
        } else {
            delay(8);
            x = 1000 + r;
        }
        sync2(&sense);
    }
}

int main(void)
{
    rt_run_on(2, work, 0);
    printf("lrsc_clear: %u of %d SCs succeeded after a remote store\n", (unsigned)bad, ROUNDS);
    return bad != 0;
}
