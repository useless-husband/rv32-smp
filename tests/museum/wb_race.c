/* Bug museum 2: a request crossing a write-back.
 * Hart 0 dirties line X, then touches two lines of the same cache set, so X
 * is evicted into the write-back buffer; the buffer is written to memory
 * only after the new line's fill.  Hart 1 reads X at varying moments.  If
 * its BusRd arrives while X sits in the buffer, the buffer must answer (it
 * holds the only current copy); with BUG_WB_NO_SNOOP memory answers with
 * the old value. */
#include "museum.h"

#define ROUNDS 60
#define SET_STRIDE 2048            /* 128 sets x 16 bytes: same set, other tag */
static volatile uint32_t area[3 * SET_STRIDE / 4] __attribute__((aligned(SET_STRIDE)));
static volatile uint32_t stale;

static void work(int hart, void *arg)
{
    (void)arg;
    uint32_t sense = 0;
    volatile uint32_t *x = &area[0], *y = &area[SET_STRIDE / 4], *z = &area[2 * SET_STRIDE / 4];
    for (uint32_t r = 1; r <= ROUNDS; r++) {
        if (hart == 0) *x = r;         /* X is M in hart 0 */
        sync2(&sense);
        if (hart == 0) {
            (void)*y;                  /* two more lines of the set: X is evicted */
            (void)*z;
        } else {
            delay((int)(r % 12));
            if (*x != r) stale++;
        }
        sync2(&sense);
    }
}

int main(void)
{
    rt_run_on(2, work, 0);
    printf("wb_race: %u of %d reads saw a stale value\n", (unsigned)stale, ROUNDS);
    return stale != 0;
}
