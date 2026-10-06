/* Demo program for the double-click launcher: three small workloads, each
 * measured with the core's own counters (cycles, retired instructions,
 * predictor and cache events). */
#include "rt.h"

#define N_SORT 400

static unsigned rng_state = 12345;
static unsigned rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

static int count_primes(int limit)
{
    static unsigned char composite[10001];
    int n = 0;
    for (int i = 2; i <= limit; i++) {
        if (composite[i])
            continue;
        n++;
        for (int j = i * i; j <= limit; j += i)
            composite[j] = 1;
    }
    return n;
}

static void quicksort(int *a, int lo, int hi)
{
    while (lo < hi) {
        int p = a[(lo + hi) / 2], i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) {
                int t = a[i];
                a[i++] = a[j];
                a[j--] = t;
            }
        }
        if (j - lo < hi - i) {
            quicksort(a, lo, j);
            lo = i;
        } else {
            quicksort(a, i, hi);
            hi = j;
        }
    }
}

/* Mandelbrot set in 16.16 fixed point: multiplies on every step. */
static void mandelbrot(void)
{
    const char *shade = " .:-=+*#%@";
    for (int y = 0; y < 18; y++) {
        for (int x = 0; x < 60; x++) {
            int cr = -137626 + x * (183501 / 60), ci = -65536 + y * (2 * 65536 / 18); /* -2.1..0.7, -1..1 */
            int zr = 0, zi = 0, k = 0;
            while (k < 30) {
                int zr2 = (int)(((long long)zr * zr) >> 16), zi2 = (int)(((long long)zi * zi) >> 16);
                if (zr2 + zi2 > 4 * 65536)
                    break;
                zi = (int)(((long long)zr * zi) >> 15) + ci;
                zr = zr2 - zi2 + cr;
                k++;
            }
            putchar(shade[k * 9 / 30]);
        }
        putchar('\n');
    }
}

int main(void)
{
    static int a[N_SORT];
    printf("rv32-pipeline demo: three small programs, each measured by the core's counters\n\n");

    rt_stats_begin();
    int primes = count_primes(10000);
    rt_stats_end("sieve");
    printf("primes below 10000: %d (expected 1229)\n\n", primes);

    for (int i = 0; i < N_SORT; i++)
        a[i] = (int)(rnd() % 100000);
    rt_stats_begin();
    quicksort(a, 0, N_SORT - 1);
    rt_stats_end("quicksort");
    int sorted = 1;
    for (int i = 1; i < N_SORT; i++)
        sorted &= a[i - 1] <= a[i];
    printf("sorted %d numbers: %s\n\n", N_SORT, sorted ? "ok" : "WRONG");

    rt_stats_begin();
    mandelbrot();
    rt_stats_end("mandelbrot");
    return (primes == 1229 && sorted) ? 0 : 1;
}
