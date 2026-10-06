/* Parallel benchmarks for the speedup and coherence-traffic measurements.
 *
 *   mandelbrot   a fixed-point Mandelbrot escape-count over a WxH grid, rows
 *                split round-robin across the harts (little sharing: each hart
 *                writes its own rows of the output, reads none of another's).
 *                The checksum of every cell is reduced at the end so the work
 *                cannot be optimised away and a wrong split is caught.
 *   histogram    a memory-sharing-heavy kernel: every hart scans the same
 *                input and bumps a SHARED 16-bucket histogram with amoadd, so
 *                the buckets ping-pong between caches.  The total count is
 *                checked.
 *
 * The program runs whichever harts exist; tools/bench.py runs the same binary
 * on the 1-, 2- and 4-hart builds and compares total machine cycles. */
#include "rt.h"
#include "smp.h"

#define W 80
#define H 48
#define ITERS 50
#define HIST_N 4096
#define BUCKETS 16

static volatile uint32_t csum[RT_MAX_HARTS][RT_LINE / 4];   /* padded per-hart partial sums */
static volatile uint32_t hist[BUCKETS];                     /* shared, contended */

static int escape(int cr, int ci)
{
    int zr = 0, zi = 0, k = 0;
    while (k < ITERS) {
        int zr2 = (int)(((long long)zr * zr) >> 16), zi2 = (int)(((long long)zi * zi) >> 16);
        if (zr2 + zi2 > 4 * 65536) break;
        zi = (int)(((long long)zr * zi) >> 15) + ci;
        zr = zr2 - zi2 + cr;
        k++;
    }
    return k;
}

static void mandel(int hart, void *arg)
{
    (void)arg;
    int n = rt_nharts();
    uint32_t s = 0;
    for (int y = hart; y < H; y += n) {
        int ci = -65536 + y * (2 * 65536 / H);
        for (int x = 0; x < W; x++) {
            int cr = -137626 + x * (183501 / W);
            s += (uint32_t)escape(cr, ci);
        }
    }
    csum[hart][0] = s;
}

static void hgram(int hart, void *arg)
{
    (void)arg;
    int n = rt_nharts();
    uint32_t x = 0x2545f491u + (uint32_t)hart;
    for (int i = hart; i < HIST_N; i += n) {
        x = x * 1664525u + 1013904223u;
        amo_add(&hist[(x >> 12) & (BUCKETS - 1)], 1);
    }
}

int main(void)
{
    int n = rt_nharts();

    uint64_t c0 = rdcycle64();
    rt_run_all(mandel, 0);
    uint32_t mc = (uint32_t)(rdcycle64() - c0);
    uint32_t sum = 0;
    for (int h = 0; h < n; h++) sum += csum[h][0];

    uint64_t c1 = rdcycle64();
    rt_run_all(hgram, 0);
    uint32_t hc = (uint32_t)(rdcycle64() - c1);
    uint32_t tot = 0;
    for (int b = 0; b < BUCKETS; b++) tot += hist[b];

    printf("bench harts=%d mandel_cycles=%u mandel_csum=%u hist_cycles=%u hist_total=%u\n", n, (unsigned)mc,
           (unsigned)sum, (unsigned)hc, (unsigned)tot);
    return tot == HIST_N ? 0 : 1;
}
