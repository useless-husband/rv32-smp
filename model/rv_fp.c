/* Integer-only IEEE 754 arithmetic for the golden model.  See rv_fp.h.
 *
 * Every finite nonzero operand is unpacked into (sign, e, m) with the
 * leading one of the significand at bit 63 of m: value = m * 2^(e - 63).
 * Each operation computes an exact (or exact-plus-sticky) result in that
 * form and hands it to round_pack(), the only place that rounds, detects
 * overflow/underflow and builds the bit pattern. */
#include "rv_fp.h"

typedef struct { uint64_t hi, lo; } u128;

enum { C_ZERO, C_FIN, C_INF, C_QNAN, C_SNAN };

typedef struct {
    int sign, cls;
    int32_t e;
    uint64_t m;
} unp;

/* ------------------------------------------------------------ 128-bit */

static u128 mk(uint64_t hi, uint64_t lo) { u128 r; r.hi = hi; r.lo = lo; return r; }

static int clz64(uint64_t x)
{
    int n = 0;
    if (!x) return 64;
    if (!(x >> 32)) { n += 32; x <<= 32; }
    if (!(x >> 48)) { n += 16; x <<= 16; }
    if (!(x >> 56)) { n += 8; x <<= 8; }
    if (!(x >> 60)) { n += 4; x <<= 4; }
    if (!(x >> 62)) { n += 2; x <<= 2; }
    if (!(x >> 63)) { n += 1; }
    return n;
}

static u128 add128(u128 a, u128 b)
{
    u128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}

static u128 sub128(u128 a, u128 b)
{
    u128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}

static int ge128(u128 a, u128 b) { return a.hi > b.hi || (a.hi == b.hi && a.lo >= b.lo); }

static u128 shl128(u128 a, int n) /* 0 <= n < 128 */
{
    if (n == 0) return a;
    if (n >= 64) return mk(a.lo << (n - 64), 0);
    return mk(a.hi << n | a.lo >> (64 - n), a.lo << n);
}

/* shift right; bits shifted out are ORed into bit 0 ("jamming") */
static u128 shrjam128(u128 a, int n)
{
    u128 r;
    uint64_t lost;
    if (n <= 0) return a;
    if (n >= 128) return mk(0, (a.hi | a.lo) != 0);
    if (n >= 64) {
        lost = a.lo | (n > 64 ? a.hi << (128 - n) : 0);
        r = mk(0, n == 64 ? a.hi : a.hi >> (n - 64));
    } else {
        lost = a.lo << (64 - n);
        r = mk(a.hi >> n, a.hi << (64 - n) | a.lo >> n);
    }
    r.lo |= (lost != 0);
    return r;
}

static u128 mul64(uint64_t a, uint64_t b)
{
    uint64_t a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    return mk(p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (p00 & 0xffffffffu));
}

/* ------------------------------------------------------------- formats */

#define P(d)    ((d) ? 53 : 24)          /* precision */
#define EMIN(d) ((d) ? -1022 : -126)
#define EMAX(d) ((d) ? 1023 : 127)
#define EBITS(d) ((d) ? 11 : 8)

static uint64_t pack(int d, int sign, uint32_t expfield, uint64_t frac)
{
    return (uint64_t)sign << (d ? 63 : 31) | (uint64_t)expfield << (P(d) - 1) | frac;
}

static uint64_t qnan(int d) { return d ? 0x7ff8000000000000ull : 0x7fc00000ull; }
static uint64_t inf(int d, int sign) { return pack(d, sign, (1u << EBITS(d)) - 1, 0); }
static uint64_t zero(int d, int sign) { return pack(d, sign, 0, 0); }

static unp unpack(int d, uint64_t v)
{
    unp u;
    int p = P(d);
    uint64_t frac = v & ((1ull << (p - 1)) - 1);
    uint32_t ex = (uint32_t)(v >> (p - 1)) & ((1u << EBITS(d)) - 1);
    u.sign = (int)((v >> (d ? 63 : 31)) & 1);
    u.e = 0;
    u.m = 0;
    if (ex == (1u << EBITS(d)) - 1) {
        u.cls = !frac ? C_INF : ((frac >> (p - 2)) & 1) ? C_QNAN : C_SNAN;
    } else if (ex == 0) {
        if (!frac) {
            u.cls = C_ZERO;
        } else { /* subnormal: normalise */
            int sh = clz64(frac);
            u.cls = C_FIN;
            u.m = frac << sh;
            u.e = EMIN(d) - (sh - (64 - p));
        }
    } else {
        u.cls = C_FIN;
        u.m = ((1ull << (p - 1)) | frac) << (64 - p);
        u.e = (int32_t)ex - EMAX(d);
    }
    return u;
}

static int is_nan(const unp *u) { return u->cls == C_QNAN || u->cls == C_SNAN; }

/* does the tail (guard bit g, everything below it s) round the magnitude up? */
static int round_up(int rm, int sign, int lsb, int g, int s)
{
    switch (rm) {
    case RV_RNE: return g && (s || lsb);
    case RV_RTZ: return 0;
    case RV_RDN: return sign && (g || s);
    case RV_RUP: return !sign && (g || s);
    default:     return g; /* RMM */
    }
}

/* Round m * 2^(e-63) (m normalised; `sticky` says nonzero bits lie below m)
 * to format d. */
static uint64_t round_pack(int d, int sign, int32_t e, uint64_t m, int sticky, int rm, uint32_t *fl)
{
    int p = P(d), rb = 64 - p, tiny = 0;
    uint64_t half = 1ull << (rb - 1), mask = (1ull << rb) - 1, sig, tail;
    int g, s, up;

    m |= (uint64_t)(sticky != 0); /* rb >= 11: bit 0 is far below the guard bit */
    if (e < EMIN(d)) {
        /* Tininess after rounding: would the result still be below 2^emin
         * if the exponent range were unbounded?  Only a value one binade
         * down whose significand rounds up to 2.0 escapes. */
        int sh = EMIN(d) - e;
        tail = m & mask;
        sig = m >> rb;
        up = round_up(rm, sign, 1, (tail & half) != 0, (tail & (half - 1)) != 0);
        tiny = !(e == EMIN(d) - 1 && sig == (1ull << p) - 1 && up);
        m = sh >= 64 ? (uint64_t)(m != 0) : (m >> sh) | ((m << (64 - sh)) != 0);
        e = EMIN(d);
    }
    tail = m & mask;
    sig = m >> rb;
    g = (tail & half) != 0;
    s = (tail & (half - 1)) != 0;
    up = round_up(rm, sign, (int)(sig & 1), g, s);
    sig += (uint64_t)up;
    if (sig >> p) { /* 1.11..1 rounded up to 10.0 */
        sig >>= 1;
        e++;
    }
    if (g || s) {
        *fl |= RV_NX;
        if (tiny) *fl |= RV_UF;
    }
    if (e > EMAX(d)) {
        *fl |= RV_OF | RV_NX;
        if (rm == RV_RTZ || (rm == RV_RDN && !sign) || (rm == RV_RUP && sign))
            return pack(d, sign, (1u << EBITS(d)) - 2, (1ull << (p - 1)) - 1); /* largest finite */
        return inf(d, sign);
    }
    if (!(sig >> (p - 1)))
        return pack(d, sign, 0, sig); /* subnormal or zero */
    return pack(d, sign, (uint32_t)(e + EMAX(d)), sig & ((1ull << (p - 1)) - 1));
}

/* x + y for two nonzero finite numbers X * 2^(ex-127), Y * 2^(ey-127)
 * (bit 127 set in both), rounded to format d. */
static uint64_t add_round(int d, int sx, int32_t ex, u128 X, int sy, int32_t ey, u128 Y, int rm, uint32_t *fl)
{
    u128 S;
    if (ex < ey || (ex == ey && !ge128(X, Y))) {
        u128 t = X; int ts = sx; int32_t te = ex;
        X = Y; sx = sy; ex = ey;
        Y = t; sy = ts; ey = te;
    }
    Y = shrjam128(Y, ex - ey > 200 ? 200 : ex - ey);
    if (sx == sy) {
        S = add128(X, Y);
        if (!ge128(S, X)) { /* carry out of bit 127 */
            S.lo = S.lo >> 1 | S.hi << 63 | (S.lo & 1);
            S.hi = S.hi >> 1 | 1ull << 63;
            ex++;
        }
    } else {
        int lz;
        S = sub128(X, Y);
        if (!(S.hi | S.lo))
            return zero(d, rm == RV_RDN); /* exact cancellation */
        lz = S.hi ? clz64(S.hi) : 64 + clz64(S.lo);
        S = shl128(S, lz);
        ex -= lz;
    }
    return round_pack(d, sx, ex, S.hi, S.lo != 0, rm, fl);
}

/* ---------------------------------------------------------- arithmetic */

static uint64_t addsub(int d, uint64_t a, uint64_t b, int sub, int rm, uint32_t *fl)
{
    unp x = unpack(d, a), y = unpack(d, b);
    y.sign ^= sub;
    if (is_nan(&x) || is_nan(&y)) {
        if (x.cls == C_SNAN || y.cls == C_SNAN) *fl |= RV_NV;
        return qnan(d);
    }
    if (x.cls == C_INF) {
        if (y.cls == C_INF && x.sign != y.sign) { *fl |= RV_NV; return qnan(d); }
        return inf(d, x.sign);
    }
    if (y.cls == C_INF) return inf(d, y.sign);
    if (x.cls == C_ZERO && y.cls == C_ZERO)
        return zero(d, x.sign == y.sign ? x.sign : rm == RV_RDN);
    if (x.cls == C_ZERO) return b ^ ((uint64_t)sub << (d ? 63 : 31));
    if (y.cls == C_ZERO) return a;
    return add_round(d, x.sign, x.e, mk(x.m, 0), y.sign, y.e, mk(y.m, 0), rm, fl);
}

uint64_t rvfp_add(int d, uint64_t a, uint64_t b, int rm, uint32_t *fl) { return addsub(d, a, b, 0, rm, fl); }
uint64_t rvfp_sub(int d, uint64_t a, uint64_t b, int rm, uint32_t *fl) { return addsub(d, a, b, 1, rm, fl); }

/* exact product of two finite nonzero numbers: P * 2^(*e - 127), bit 127 set */
static u128 product(const unp *x, const unp *y, int32_t *e)
{
    u128 pr = mul64(x->m, y->m);
    if (pr.hi >> 63) {
        *e = x->e + y->e + 1;
    } else {
        pr = shl128(pr, 1);
        *e = x->e + y->e;
    }
    return pr;
}

uint64_t rvfp_mul(int d, uint64_t a, uint64_t b, int rm, uint32_t *fl)
{
    unp x = unpack(d, a), y = unpack(d, b);
    int sign = x.sign ^ y.sign;
    int32_t e;
    u128 pr;
    if (is_nan(&x) || is_nan(&y)) {
        if (x.cls == C_SNAN || y.cls == C_SNAN) *fl |= RV_NV;
        return qnan(d);
    }
    if (x.cls == C_INF || y.cls == C_INF) {
        if (x.cls == C_ZERO || y.cls == C_ZERO) { *fl |= RV_NV; return qnan(d); }
        return inf(d, sign);
    }
    if (x.cls == C_ZERO || y.cls == C_ZERO) return zero(d, sign);
    pr = product(&x, &y, &e);
    return round_pack(d, sign, e, pr.hi, pr.lo != 0, rm, fl);
}

uint64_t rvfp_fma(int d, uint64_t a, uint64_t b, uint64_t c, int neg_prod, int neg_c, int rm, uint32_t *fl)
{
    unp x = unpack(d, a), y = unpack(d, b), z = unpack(d, c);
    int sp = x.sign ^ y.sign ^ (neg_prod != 0), sc = z.sign ^ (neg_c != 0);
    int inf_zero = (x.cls == C_INF && y.cls == C_ZERO) || (x.cls == C_ZERO && y.cls == C_INF);
    int32_t e;
    u128 pr;
    if (x.cls == C_SNAN || y.cls == C_SNAN || z.cls == C_SNAN || inf_zero)
        *fl |= RV_NV; /* inf * 0 is invalid even when the addend is a quiet NaN */
    if (is_nan(&x) || is_nan(&y) || is_nan(&z) || inf_zero)
        return qnan(d);
    if (x.cls == C_INF || y.cls == C_INF) {
        if (z.cls == C_INF && sp != sc) { *fl |= RV_NV; return qnan(d); }
        return inf(d, sp);
    }
    if (z.cls == C_INF) return inf(d, sc);
    if (x.cls == C_ZERO || y.cls == C_ZERO) {
        if (z.cls == C_ZERO) return zero(d, sp == sc ? sp : rm == RV_RDN);
        return c ^ ((uint64_t)(neg_c != 0) << (d ? 63 : 31));
    }
    pr = product(&x, &y, &e);
    if (z.cls == C_ZERO)
        return round_pack(d, sp, e, pr.hi, pr.lo != 0, rm, fl);
    return add_round(d, sp, e, pr, sc, z.e, mk(z.m, 0), rm, fl);
}

uint64_t rvfp_div(int d, uint64_t a, uint64_t b, int rm, uint32_t *fl)
{
    unp x = unpack(d, a), y = unpack(d, b);
    int sign = x.sign ^ y.sign, carry = 0;
    int32_t e;
    uint64_t r, q = 0;
    if (is_nan(&x) || is_nan(&y)) {
        if (x.cls == C_SNAN || y.cls == C_SNAN) *fl |= RV_NV;
        return qnan(d);
    }
    if (x.cls == C_INF) {
        if (y.cls == C_INF) { *fl |= RV_NV; return qnan(d); }
        return inf(d, sign);
    }
    if (y.cls == C_INF) return zero(d, sign);
    if (y.cls == C_ZERO) {
        if (x.cls == C_ZERO) { *fl |= RV_NV; return qnan(d); }
        *fl |= RV_DZ;
        return inf(d, sign);
    }
    if (x.cls == C_ZERO) return zero(d, sign);
    /* long division, one quotient bit per step; the first bit is always 1 */
    e = x.e - y.e;
    r = x.m;
    if (r < y.m) {
        carry = (int)(r >> 63);
        r <<= 1;
        e--;
    }
    for (int i = 0; i < 64; i++) {
        int bit = carry || r >= y.m;
        if (bit) r -= y.m;
        q = q << 1 | (uint64_t)bit;
        carry = (int)(r >> 63);
        r <<= 1;
    }
    return round_pack(d, sign, e, q, r != 0 || carry, rm, fl);
}

uint64_t rvfp_sqrt(int d, uint64_t a, int rm, uint32_t *fl)
{
    unp x = unpack(d, a);
    u128 rad, rem = mk(0, 0);
    uint64_t root = 0;
    int32_t e;
    if (is_nan(&x)) {
        if (x.cls == C_SNAN) *fl |= RV_NV;
        return qnan(d);
    }
    if (x.cls == C_ZERO) return a;
    if (x.sign) { *fl |= RV_NV; return qnan(d); }
    if (x.cls == C_INF) return a;
    /* make the exponent even: radicand in [1,4) scaled by 2^126 */
    e = x.e;
    if (e & 1) {
        rad = mk(x.m, 0); /* 2m * 2^63 */
        e -= 1;
    } else {
        rad = mk(x.m >> 1, x.m << 63); /* m * 2^63 */
    }
    /* digit-by-digit square root: two radicand bits in, one root bit out */
    for (int i = 0; i < 64; i++) {
        u128 trial = mk(root >> 62, root << 2 | 1);
        rem = shl128(rem, 2);
        rem.lo |= rad.hi >> 62;
        rad = shl128(rad, 2);
        if (ge128(rem, trial)) {
            rem = sub128(rem, trial);
            root = root << 1 | 1;
        } else {
            root <<= 1;
        }
    }
    return round_pack(d, 0, e / 2, root, (rem.hi | rem.lo) != 0, rm, fl);
}

/* ----------------------------------------------- comparisons, min/max */

/* a < b for two non-NaN values, +0 and -0 equal */
static int lt_nonan(const unp *x, const unp *y, int d, uint64_t a, uint64_t b)
{
    uint64_t mask = d ? 0x7fffffffffffffffull : 0x7fffffffull;
    uint64_t ma = a & mask, mb = b & mask;
    if (x->cls == C_ZERO && y->cls == C_ZERO) return 0;
    if (x->sign != y->sign) return x->sign;
    return x->sign ? ma > mb : ma < mb;
}

int rvfp_eq(int d, uint64_t a, uint64_t b, uint32_t *fl)
{
    unp x = unpack(d, a), y = unpack(d, b);
    if (is_nan(&x) || is_nan(&y)) {
        if (x.cls == C_SNAN || y.cls == C_SNAN) *fl |= RV_NV;
        return 0;
    }
    return (x.cls == C_ZERO && y.cls == C_ZERO) || a == b;
}

int rvfp_lt(int d, uint64_t a, uint64_t b, uint32_t *fl)
{
    unp x = unpack(d, a), y = unpack(d, b);
    if (is_nan(&x) || is_nan(&y)) { *fl |= RV_NV; return 0; }
    return lt_nonan(&x, &y, d, a, b);
}

int rvfp_le(int d, uint64_t a, uint64_t b, uint32_t *fl)
{
    unp x = unpack(d, a), y = unpack(d, b);
    if (is_nan(&x) || is_nan(&y)) { *fl |= RV_NV; return 0; }
    return !lt_nonan(&y, &x, d, b, a);
}

uint64_t rvfp_minmax(int d, uint64_t a, uint64_t b, int is_max, uint32_t *fl)
{
    unp x = unpack(d, a), y = unpack(d, b);
    int a_less;
    if (x.cls == C_SNAN || y.cls == C_SNAN) *fl |= RV_NV;
    if (is_nan(&x) && is_nan(&y)) return qnan(d);
    if (is_nan(&x)) return b;
    if (is_nan(&y)) return a;
    a_less = lt_nonan(&x, &y, d, a, b) || (x.cls == C_ZERO && y.cls == C_ZERO && x.sign && !y.sign);
    return (a_less != (is_max != 0)) ? a : b;
}

uint32_t rvfp_classify(int d, uint64_t a)
{
    unp x = unpack(d, a);
    int sub = x.cls == C_FIN && x.e < EMIN(d);
    switch (x.cls) {
    case C_INF:  return x.sign ? 1u << 0 : 1u << 7;
    case C_ZERO: return x.sign ? 1u << 3 : 1u << 4;
    case C_SNAN: return 1u << 8;
    case C_QNAN: return 1u << 9;
    default:
        if (sub) return x.sign ? 1u << 2 : 1u << 5;
        return x.sign ? 1u << 1 : 1u << 6;
    }
}

/* ---------------------------------------------------------- conversions */

uint64_t rvfp_f2f(int to_d, uint64_t a, int rm, uint32_t *fl)
{
    unp x = unpack(!to_d, a);
    if (is_nan(&x)) {
        if (x.cls == C_SNAN) *fl |= RV_NV;
        return qnan(to_d);
    }
    if (x.cls == C_INF) return inf(to_d, x.sign);
    if (x.cls == C_ZERO) return zero(to_d, x.sign);
    return round_pack(to_d, x.sign, x.e, x.m, 0, rm, fl);
}

uint64_t rvfp_i2f(int to_d, uint32_t v, int is_unsigned, int rm, uint32_t *fl)
{
    int sign = !is_unsigned && (v >> 31);
    uint64_t mag = sign ? (uint64_t)(0u - v) : v;
    int lz;
    if (!mag) return zero(to_d, 0);
    lz = clz64(mag);
    return round_pack(to_d, sign, 63 - lz, mag << lz, 0, rm, fl);
}

uint32_t rvfp_f2i(int from_d, uint64_t a, int is_unsigned, int rm, uint32_t *fl)
{
    unp x = unpack(from_d, a);
    uint32_t pos_max = is_unsigned ? 0xffffffffu : 0x7fffffffu;
    uint32_t neg_max = is_unsigned ? 0u : 0x80000000u;
    uint64_t mag = 0, frac;
    int g = 0, s = 0;
    if (is_nan(&x)) { *fl |= RV_NV; return pos_max; }
    if (x.cls == C_INF) { *fl |= RV_NV; return x.sign ? neg_max : pos_max; }
    if (x.cls == C_ZERO) return 0;
    if (x.e >= 32) { *fl |= RV_NV; return x.sign ? neg_max : pos_max; }
    if (x.e < -1) {
        s = 1; /* below one half */
    } else {
        int sh = 63 - x.e; /* 32..64 */
        mag = sh == 64 ? 0 : x.m >> sh;
        frac = sh == 64 ? x.m : x.m << (64 - sh);
        g = (int)(frac >> 63);
        s = (frac << 1) != 0;
    }
    mag += (uint64_t)round_up(rm, x.sign, (int)(mag & 1), g, s);
    if (is_unsigned ? (x.sign ? mag != 0 : mag > 0xffffffffull)
                    : (x.sign ? mag > 0x80000000ull : mag > 0x7fffffffull)) {
        *fl |= RV_NV;
        return x.sign ? neg_max : pos_max;
    }
    if (g || s) *fl |= RV_NX;
    return x.sign ? 0u - (uint32_t)mag : (uint32_t)mag;
}
