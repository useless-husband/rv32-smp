/* Bug museum 3: an LR reservation that survives a remote store.
 * Hart 0 loads X with LR and says so; hart 1 then stores to X and raises a
 * flag; only then does hart 0 try SC, so the store is always between them.  The SC must fail;
 * with BUG_LRSC_NO_CLEAR the remote invalidation leaves the reservation in
 * place and the SC succeeds, overwriting hart 1's store - the atomicity an
 * LR/SC pair promises is gone. */
#include "museum.h"

#define ROUNDS 20
static volatile uint32_t x __attribute__((aligned(16)));
static volatile uint32_t flag __attribute__((aligned(16)));
static volatile uint32_t lr_done __attribute__((aligned(16)));
static volatile uint32_t bad;

static void work(int hart, void *arg)
{
    (void)arg;
    uint32_t sense = 0;
    for (uint32_t r = 1; r <= ROUNDS; r++) {
        if (hart == 0) { flag = 0; lr_done = 0; }
        sync2(&sense);
        if (hart == 0) {
            uint32_t v = lr_w(&x);
            store_release(&lr_done, 1);        /* hart 1 stores only after the LR */
            while (!load_acquire(&flag)) { }   /* ... and before the SC */
            uint32_t fail = sc_w(&x, v + 1);
            if (!fail) bad++;          /* x was written by hart 1 since the LR */
        } else {
            while (!load_acquire(&lr_done)) { }
            x = 1000 + r;
            store_release(&flag, 1);
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
