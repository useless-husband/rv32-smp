/* Bug museum 4: Exclusive granted although another cache has a copy.
 * Hart 0 reads X, then hart 1 reads X.  Hart 1 must get S (hart 0 still
 * holds the line); with BUG_E_IGNORES_SHARED it takes E, so its next store
 * is a silent E -> M upgrade with no invalidation, and hart 0 keeps reading
 * its old S copy. */
#include "museum.h"

#define ROUNDS 20
static volatile uint32_t x __attribute__((aligned(16)));
static volatile uint32_t stale;

static void work(int hart, void *arg)
{
    (void)arg;
    uint32_t sense = 0;
    for (uint32_t r = 1; r <= ROUNDS; r++) {
        if (hart == 0) (void)x;        /* hart 0 first: E, or S */
        sync2(&sense);
        if (hart == 1) {
            (void)x;                   /* hart 1 second: must be S */
            x = r;                     /* needs BusUpgr from S */
        }
        sync2(&sense);
        if (hart == 0 && x != r) stale++;
        sync2(&sense);
    }
}

int main(void)
{
    rt_run_on(2, work, 0);
    printf("e_shared: %u of %d reads saw a stale value\n", (unsigned)stale, ROUNDS);
    return stale != 0;
}
