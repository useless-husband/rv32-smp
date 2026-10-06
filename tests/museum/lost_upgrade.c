/* Bug museum 1: the lost upgrade race.
 * Both harts hold the line in S, then each writes a different word of it.
 * Both caches ask for BusUpgr; the bus serves one first and the other loses
 * its S copy while it waits.  The correct cache turns its request into
 * BusRdX and gets the winner's data; with BUG_LOST_UPGRADE it upgrades the
 * stale copy it no longer has, and the winner's word is lost. */
#include "museum.h"

#define ROUNDS 64
static volatile uint32_t line[4] __attribute__((aligned(16)));
static volatile uint32_t lost;

static void work(int hart, void *arg)
{
    (void)arg;
    uint32_t sense = 0;
    for (uint32_t r = 1; r <= ROUNDS; r++) {
        (void)line[2];                 /* both harts take an S copy */
        sync2(&sense);
        int skew = (int)(r % 32) - 16;     /* sweep the relative timing of the two stores */
        delay(hart ? skew : -skew);
        line[hart] = r;                /* both upgrade at nearly the same time */
        sync2(&sense);
        if (hart == 0 && (line[0] != r || line[1] != r)) lost++;
        sync2(&sense);
    }
}

int main(void)
{
    rt_run_on(2, work, 0);
    printf("lost_upgrade: %u of %d rounds lost a write\n", (unsigned)lost, ROUNDS);
    return lost != 0;
}
