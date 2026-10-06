/* Atomic counter stress: every hart adds to shared counters through each
 * primitive of smp.h, then hart 0 checks the totals.  A lost update in any
 * of them (a broken AMO, an SC that succeeds when it must fail, a lock that
 * lets two harts in) makes a total come out short. */
#include "rt.h"
#include "smp.h"

#define ITER 200

static volatile uint32_t c_amo, c_lrsc, c_spin_amo, c_spin_lrsc, c_tick_amo, c_tick_lrsc;
static volatile uint32_t l_amo __attribute__((aligned(16)));
static volatile uint32_t l_lrsc __attribute__((aligned(16)));
static ticket_lock t_amo __attribute__((aligned(16)));
static ticket_lock t_lrsc __attribute__((aligned(16)));

static void work(int hart, void *arg)
{
    (void)hart;
    (void)arg;
    for (int i = 0; i < ITER; i++) {
        amo_add(&c_amo, 1);
        lrsc_add(&c_lrsc, 1);
        spin_lock_amo(&l_amo);
        c_spin_amo = c_spin_amo + 1;
        spin_unlock(&l_amo);
        spin_lock_lrsc(&l_lrsc);
        c_spin_lrsc = c_spin_lrsc + 1;
        spin_unlock(&l_lrsc);
        ticket_lock_amo(&t_amo);
        c_tick_amo = c_tick_amo + 1;
        ticket_unlock(&t_amo);
        ticket_lock_lrsc(&t_lrsc);
        c_tick_lrsc = c_tick_lrsc + 1;
        ticket_unlock(&t_lrsc);
    }
}

int main(void)
{
    int n = rt_nharts();
    rt_run_all(work, 0);
    uint32_t want = (uint32_t)(n * ITER);
    int bad = 0;
    const char *name[6] = {"amoadd", "lr/sc add", "spinlock (amoswap)", "spinlock (lr/sc)", "ticket (amoadd)",
                           "ticket (lr/sc)"};
    uint32_t got[6] = {c_amo, c_lrsc, c_spin_amo, c_spin_lrsc, c_tick_amo, c_tick_lrsc};
    for (int i = 0; i < 6; i++) {
        printf("%-20s %u / %u\n", name[i], (unsigned)got[i], (unsigned)want);
        if (got[i] != want) bad = 1;
    }
    printf(bad ? "atomics: FAIL\n" : "atomics: ok on %d harts\n", n);
    return bad;
}
