/* Minimal bare-metal runtime (see rt.h).  Output goes to the console
 * register, exit() writes the EXIT register. */
#include "rt.h"

#include "rv_platform.h"

#define CONSOLE (*(volatile uint32_t *)RV_MMIO_CONSOLE)
#define EXITREG (*(volatile uint32_t *)RV_MMIO_EXIT)

int putchar(int c)
{
    CONSOLE = (uint8_t)c;
    return c;
}

int puts(const char *s)
{
    while (*s)
        putchar(*s++);
    putchar('\n');
    return 0;
}

void exit(int code)
{
    EXITREG = ((uint32_t)code << 1) | 1;
    for (;;)
        ;
}

void rt_unexpected_trap(uint32_t cause, uint32_t epc, uint32_t tval, uint32_t hart)
{
    printf("\nunexpected trap on hart %u: mcause=%u mepc=0x%08x mtval=0x%08x\n", (unsigned)hart, (unsigned)cause,
           (unsigned)epc, (unsigned)tval);
    exit(99);
}

/* ---------------------------------------------------------------- libc */

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if ((((uintptr_t)p | (uintptr_t)q) & 3) == 0) {
        for (; n >= 4; n -= 4, p += 4, q += 4)
            *(uint32_t *)p = *(const uint32_t *)q;
    }
    while (n--)
        *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q)
        return memcpy(d, s, n);
    while (n--)
        p[n] = q[n];
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

/* 64-bit division helpers the compiler calls on RV32 (no libgcc here). */
uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = r << 1 | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) { return __udivmoddi4(n, d, 0); }

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    __udivmoddi4(n, d, &r);
    return r;
}

/* -------------------------------------------------------------- printf */

static void out_num(uint64_t v, unsigned base, int upper, int width, char pad, int neg)
{
    char buf[24];
    int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        if (v >> 32) {
            buf[n++] = dig[v % base];
            v /= base;
        } else { /* 32-bit fast path: hardware divide instead of the 64-bit helper */
            uint32_t w = (uint32_t)v;
            buf[n++] = dig[w % base];
            v = w / base;
        }
    } while (v);
    if (neg && pad == '0') {
        putchar('-');
        width--;
    } else if (neg) {
        buf[n++] = '-';
    }
    while (width-- > n)
        putchar(pad);
    while (n)
        putchar(buf[--n]);
}

int vprintf(const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') {
            putchar(*f);
            continue;
        }
        f++;
        char pad = ' ';
        int width = 0, lng = 0, left = 0;
        if (*f == '-') { left = 1; f++; }
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        while (*f == 'l') { lng++; f++; }
        if (*f == 'z') { f++; }
        switch (*f) {
        case 'd': case 'i': {
            int64_t v = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
            out_num(v < 0 ? 0 - (uint64_t)v : (uint64_t)v, 10, 0, width, pad, v < 0);
            break;
        }
        case 'u': case 'x': case 'X': {
            uint64_t v = lng >= 2 ? va_arg(ap, unsigned long long)
                         : lng   ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            out_num(v, *f == 'u' ? 10 : 16, *f == 'X', width, pad, 0);
            break;
        }
        case 'p':
            putchar('0'); putchar('x');
            out_num((uintptr_t)va_arg(ap, void *), 16, 0, 8, '0', 0);
            break;
        case 'c':
            putchar(va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = (int)strlen(s);
            if (!left) while (width-- > len) putchar(' ');
            while (*s) putchar(*s++);
            if (left) while (width-- > len) putchar(' ');
            break;
        }
        case '%':
            putchar('%');
            break;
        default:
            putchar('%');
            putchar(*f);
            if (!*f) return 0;
        }
    }
    return 0;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    return 0;
}

/* ------------------------------------------------------------ counters */

#define HPM(n) case n: __asm__ volatile("csrr %0, mhpmcounter" #n : "=r"(v)); break
uint32_t rt_hpm(int n)
{
    uint32_t v = 0;
    switch (n) {
    HPM(3); HPM(4); HPM(5); HPM(6); HPM(7); HPM(8); HPM(9); HPM(10); HPM(11); HPM(12);
    }
    return v;
}

static uint64_t c0, i0;
static uint32_t h0[RV_HPM_COUNT];

void rt_stats_begin(void)
{
    for (int i = 0; i < RV_HPM_COUNT; i++)
        h0[i] = rt_hpm(RV_HPM_FIRST + i);
    i0 = rdinstret64();
    c0 = rdcycle64();
}

void rt_stats_end(const char *label)
{
    uint64_t c = rdcycle64() - c0, n = rdinstret64() - i0;
    static const char *names[] = RV_HPM_NAMES;
    uint32_t h[RV_HPM_COUNT];
    for (int i = 0; i < RV_HPM_COUNT; i++)
        h[i] = rt_hpm(RV_HPM_FIRST + i) - h0[i];
    uint64_t cpi1000 = n ? (c * 1000 + n / 2) / n : 0;
    printf("[%s] cycles=%llu instret=%llu CPI=%u.%03u\n[%s]", label, (unsigned long long)c,
           (unsigned long long)n, (unsigned)(cpi1000 / 1000), (unsigned)(cpi1000 % 1000), label);
    for (int i = 0; i < RV_HPM_COUNT; i++)
        printf(" %s=%u", names[i], (unsigned)h[i]);
    putchar('\n');
}
