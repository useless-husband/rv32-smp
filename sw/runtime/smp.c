/* Fork/join for bare-metal programs: hart 0 posts a function to each
 * mailbox, the other harts (waiting in rt_secondary) run it and report
 * back.  Mailboxes are one cache line each so harts do not falsely share. */
#include "smp.h"

volatile uint32_t rt_boot_released;

typedef struct {
    volatile uint32_t seq;      /* incremented by hart 0 for each new job */
    volatile uint32_t done;     /* set to seq by the hart when the job returned */
    rt_fn fn;
    void *arg;
} __attribute__((aligned(RT_LINE))) mailbox;

static mailbox box[RT_MAX_HARTS];

void rt_secondary(int hart)
{
    uint32_t seen = 0;
    for (;;) {
        uint32_t s;
        while ((s = load_acquire(&box[hart].seq)) == seen) { }
        seen = s;
        box[hart].fn(hart, box[hart].arg);
        store_release(&box[hart].done, s);
    }
}

void rt_run_on(int n, rt_fn fn, void *arg)
{
    for (int h = 1; h < n; h++) {
        box[h].fn = fn;
        box[h].arg = arg;
        store_release(&box[h].seq, box[h].seq + 1);
    }
    fn(0, arg);
    for (int h = 1; h < n; h++)
        while (load_acquire(&box[h].done) != box[h].seq) { }
}

void rt_barrier_wait(rt_barrier *b, int n, uint32_t *local_sense)
{
    uint32_t s = !*local_sense;
    *local_sense = s;
    if (amo_add(&b->count, 1) == (uint32_t)n - 1) {
        b->count = 0;
        store_release(&b->sense, s);
    } else {
        while (load_acquire(&b->sense) != s) { }
    }
}
