/* Golden model: RV32IM + Zicsr + Zifencei (+ F and D when has_fpu is set),
 * machine mode only.  See rv_iss.h and docs/DESIGN.md section 2 for the
 * exact list of what is implemented.  The floating-point arithmetic itself
 * is in rv_fp.c. */
#include "rv_iss.h"
#include "rv_fp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CAUSE_MISALIGNED_FETCH = 0,
    CAUSE_ILLEGAL = 2,
    CAUSE_BREAKPOINT = 3,
    CAUSE_MISALIGNED_LOAD = 4,
    CAUSE_LOAD_FAULT = 5,
    CAUSE_MISALIGNED_STORE = 6,
    CAUSE_STORE_FAULT = 7,
    CAUSE_ECALL_M = 11,
};

int rv_iss_init(rv_iss *s)
{
    memset(s, 0, sizeof *s);
    s->ram = calloc(1, RV_RAM_SIZE);
    if (!s->ram)
        return -1;
    s->pc = RV_RESET_PC;
    s->nharts = 1;
    return 0;
}

void rv_iss_init_shared(rv_iss *s, const rv_iss *owner, uint32_t hartid)
{
    memset(s, 0, sizeof *s);
    s->ram = owner->ram;
    s->shared_ram = 1;
    s->pc = RV_RESET_PC;
    s->hartid = hartid;
    s->nharts = owner->nharts;
}

void rv_iss_free(rv_iss *s)
{
    if (!s->shared_ram)
        free(s->ram);
    free(s->console);
    s->ram = NULL;
    s->console = NULL;
}

int rv_exit_code(uint32_t v)
{
    return v == 1 ? 0 : (int)(v >> 1);
}

static int in_ram(uint32_t a, uint32_t size)
{
    return a >= RV_RAM_BASE && a - RV_RAM_BASE <= RV_RAM_SIZE - size;
}

static int in_mmio(uint32_t a)
{
    return a >= RV_MMIO_BASE && a - RV_MMIO_BASE < RV_MMIO_SIZE;
}

static void fail(rv_iss *s, const char *what, uint32_t addr)
{
    if (!s->error) {
        s->error = 1;
        snprintf(s->errmsg, sizeof s->errmsg, "%s outside RAM and I/O at 0x%08x (pc 0x%08x)",
                 what, addr, s->pc);
    }
}

/* ------------------------------------------------------------------ ELF */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

int rv_iss_load_elf(rv_iss *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(s->errmsg, sizeof s->errmsg, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 ? malloc((size_t)len) : NULL;
    int ok = b && fread(b, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (!ok || len < 52 || memcmp(b, "\177ELF", 4) || b[4] != 1 || b[5] != 1 || rd16(b + 18) != 243) {
        snprintf(s->errmsg, sizeof s->errmsg, "%s: not a little-endian ELF32 RISC-V file", path);
        free(b);
        return -1;
    }
    uint32_t phoff = rd32(b + 28), phentsize = rd16(b + 42), phnum = rd16(b + 44);
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = b + phoff + i * phentsize;
        if ((long)(phoff + (i + 1) * phentsize) > len)
            break;
        if (rd32(ph) != 1 /* PT_LOAD */)
            continue;
        uint32_t off = rd32(ph + 4), paddr = rd32(ph + 12), filesz = rd32(ph + 16), memsz = rd32(ph + 20);
        if (memsz == 0)
            continue;
        if (!in_ram(paddr, memsz) || filesz > memsz || (long)off + (long)filesz > len) {
            snprintf(s->errmsg, sizeof s->errmsg, "%s: segment at 0x%08x (+%u) does not fit in RAM", path,
                     paddr, memsz);
            free(b);
            return -1;
        }
        memcpy(s->ram + (paddr - RV_RAM_BASE), b + off, filesz);
        memset(s->ram + (paddr - RV_RAM_BASE) + filesz, 0, memsz - filesz);
    }
    if (rd32(b + 24) != RV_RESET_PC) {
        snprintf(s->errmsg, sizeof s->errmsg, "%s: entry point 0x%08x is not the reset PC 0x%08x", path,
                 rd32(b + 24), RV_RESET_PC);
        free(b);
        return -1;
    }
    free(b);
    return 0;
}

int rv_iss_write_hex(const rv_iss *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    /* only the used part of RAM: up to the last nonzero word */
    uint32_t words = RV_RAM_SIZE / 4, last = 0;
    for (uint32_t i = 0; i < words; i++)
        if (rd32(s->ram + 4 * i))
            last = i + 1;
    fprintf(f, "@00000000\n");
    for (uint32_t i = 0; i < last; i++)
        fprintf(f, "%08x\n", rd32(s->ram + 4 * i));
    return fclose(f);
}

/* --------------------------------------------------------------- memory */

static uint32_t load(rv_iss *s, uint32_t a, int size)
{
    if (in_ram(a, (uint32_t)size)) {
        const uint8_t *p = s->ram + (a - RV_RAM_BASE);
        return size == 1 ? p[0] : size == 2 ? rd16(p) : rd32(p);
    }
    if (!in_mmio(a))
        fail(s, "load", a);
    if (a == RV_MMIO_NHARTS && size == 4)
        return s->nharts;
    return 0; /* other I/O registers read as zero */
}

/* a data access to RAM: through the hook when there are other harts */
static uint32_t dload(rv_iss *s, int kind, uint32_t a, int size, uint32_t wv, int *sc_ok)
{
    if (s->data_hook && in_ram(a, (uint32_t)size))
        return s->data_hook(s->hook_ctx, s->hartid, kind, a, size, wv, sc_ok);
    if (kind == 3)
        *sc_ok = s->resv_valid && s->resv_addr == (a & ~15u);
    return load(s, a, size);
}

static void console_put(rv_iss *s, char ch)
{
    if (s->console_len + 1 >= s->console_cap) {
        size_t cap = s->console_cap ? 2 * s->console_cap : 4096;
        char *n = realloc(s->console, cap);
        if (!n)
            return;
        s->console = n;
        s->console_cap = cap;
    }
    s->console[s->console_len++] = ch;
    s->console[s->console_len] = 0;
    if (s->echo) {
        putchar(ch);
        if (ch == '\n')
            fflush(stdout);
    }
}

static void store(rv_iss *s, rv_commit *c, uint32_t a, uint32_t v, int size)
{
    uint32_t sh = (a & 3) * 8;
    uint32_t mask = (size == 4 ? 0xfu : size == 2 ? 0x3u : 0x1u) << (a & 3);
    c->mem_we = 1;
    c->mem_addr = a & ~3u;
    c->mem_wdata = (size == 4 ? v : size == 2 ? (v & 0xffff) << sh : (v & 0xff) << sh);
    c->mem_wmask = mask;
    if (in_ram(a, (uint32_t)size)) {
        uint8_t *p = s->ram + (a - RV_RAM_BASE);
        for (int i = 0; i < size; i++)
            p[i] = (uint8_t)(v >> (8 * i));
        return;
    }
    if (!in_mmio(a)) {
        fail(s, "store", a);
        return;
    }
    /* I/O registers are word registers; byte lanes are taken from the word */
    uint32_t word = c->mem_wdata;
    if (c->mem_addr == RV_MMIO_CONSOLE && (mask & 1))
        console_put(s, (char)(word & 0xff));
    if (c->mem_addr == RV_MMIO_EXIT) {
        s->exited = 1;
        s->exit_value = word;
    }
}

/* ----------------------------------------------------------------- CSRs */

static int csr_nondet(uint32_t a)
{
    return a == 0xB00 || a == 0xB80 || a == 0xC00 || a == 0xC80 ||
           (a >= 0xB03 && a < 0xB03 + RV_HPM_COUNT) || (a >= 0xB83 && a < 0xB83 + RV_HPM_COUNT) ||
           (a >= 0xC03 && a < 0xC03 + RV_HPM_COUNT) || (a >= 0xC83 && a < 0xC83 + RV_HPM_COUNT);
}

/* Returns 0 if the CSR exists, -1 if not. */
static int csr_read(rv_iss *s, uint32_t a, uint32_t *v)
{
    uint32_t lo = a & 0x7f, user = (a & 0xf00) == 0xC00;
    switch (a) {
    case 0x001: case 0x002: case 0x003: /* fflags, frm, fcsr: only while mstatus.FS is on */
        if (!s->has_fpu || s->fs == 0) return -1;
        *v = a == 1 ? s->fflags : a == 2 ? s->frm : s->frm << 5 | s->fflags;
        return 0;
    case 0x300: /* SD (bit 31) summarises FS == dirty */
        *v = s->mie_bit << 3 | s->mpie_bit << 7 | 3u << 11 | s->fs << 13 | (uint32_t)(s->fs == 3) << 31;
        return 0;
    case 0x301: *v = s->has_fpu ? 0x40001129u : 0x40001101u; return 0; /* MXL=32, I, M, A (, F, D) */
    case 0x304: case 0x344: *v = 0; return 0; /* mie, mip: no interrupts */
    case 0x305: *v = s->mtvec; return 0;
    case 0x340: *v = s->mscratch; return 0;
    case 0x341: *v = s->mepc; return 0;
    case 0x342: *v = s->mcause; return 0;
    case 0x343: *v = s->mtval; return 0;
    case 0xF14: *v = s->hartid; return 0;
    case 0xF11: case 0xF12: case 0xF13: case 0xF15: *v = 0; return 0;
    }
    if ((a & 0xf00) == 0xB00 || user) {
        uint32_t hi = a & 0x80;
        uint64_t val;
        if (lo == 0)
            val = s->mcycle;
        else if (lo == 2)
            val = s->minstret;
        else if (lo >= RV_HPM_FIRST && lo < RV_HPM_FIRST + RV_HPM_COUNT)
            val = s->hpm[lo - RV_HPM_FIRST];
        else
            return -1;
        *v = hi ? (uint32_t)(val >> 32) : (uint32_t)val;
        return 0;
    }
    return -1;
}

/* Returns 1 if the write replaced the instret increment. */
static int csr_write(rv_iss *s, uint32_t a, uint32_t v)
{
    uint32_t lo = a & 0x7f, hi = a & 0x80;
    uint64_t *ctr = NULL;
    switch (a) {
    case 0x001: s->fflags = v & 31; s->fs = 3; return 0;
    case 0x002: s->frm = v & 7; s->fs = 3; return 0;
    case 0x003: s->fflags = v & 31; s->frm = (v >> 5) & 7; s->fs = 3; return 0;
    case 0x300:
        s->mie_bit = (v >> 3) & 1;
        s->mpie_bit = (v >> 7) & 1;
        if (s->has_fpu) s->fs = (v >> 13) & 3;
        return 0;
    case 0x301: case 0x304: case 0x344: return 0; /* WARL, nothing writable */
    case 0x305: s->mtvec = v & ~3u; return 0;
    case 0x340: s->mscratch = v; return 0;
    case 0x341: s->mepc = v & ~3u; return 0;
    case 0x342: s->mcause = v; return 0;
    case 0x343: s->mtval = v; return 0;
    }
    if (lo == 0)
        ctr = &s->mcycle;
    else if (lo == 2)
        ctr = &s->minstret;
    else
        ctr = &s->hpm[lo - RV_HPM_FIRST];
    if (hi)
        *ctr = (*ctr & 0xffffffffu) | (uint64_t)v << 32;
    else
        *ctr = (*ctr & ~(uint64_t)0xffffffffu) | v;
    return lo == 2;
}

/* --------------------------------------------------------------- execute */

static int32_t sext(uint32_t v, int bits)
{
    return (int32_t)(v << (32 - bits)) >> (32 - bits);
}

static void trap(rv_iss *s, rv_commit *c, uint32_t cause, uint32_t tval)
{
    c->trap = 1;
    c->cause = cause;
    c->rd_we = 0;
    s->mepc = s->pc;
    s->mcause = cause;
    s->mtval = tval;
    s->mpie_bit = s->mie_bit;
    s->mie_bit = 0;
    s->pc = s->mtvec;
}

static uint32_t muldiv(uint32_t f3, uint32_t a, uint32_t b)
{
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    switch (f3) {
    case 0: return a * b;
    case 1: return (uint32_t)(((int64_t)sa * (int64_t)sb) >> 32);
    case 2: return (uint32_t)(((int64_t)sa * (int64_t)(uint64_t)b) >> 32);
    case 3: return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
    case 4: return b == 0 ? 0xffffffffu : (sa == INT32_MIN && sb == -1) ? a : (uint32_t)(sa / sb);
    case 5: return b == 0 ? 0xffffffffu : a / b;
    case 6: return b == 0 ? a : (sa == INT32_MIN && sb == -1) ? 0 : (uint32_t)(sa % sb);
    default: return b == 0 ? a : a % b;
    }
}


/* ------------------------------------------------------------- F and D */

/* A single lives in the low half of a 64-bit f register with all ones above
 * it (NaN boxing); anything else read as a single is the canonical NaN. */
static uint32_t unbox(uint64_t v) { return (v >> 32) == 0xffffffffu ? (uint32_t)v : 0x7fc00000u; }
static uint64_t box(uint32_t v) { return 0xffffffff00000000ull | v; }

/* Executes one OP-FP or fused multiply-add instruction.  Returns 0 if the
 * encoding is illegal; otherwise sets fw and fres (f register result) or
 * xw and xres (integer result) and ORs the raised flags into fl. */
static int fp_exec(rv_iss *s, uint32_t in, int *fw, uint64_t *fres, int *xw, uint32_t *xres, uint32_t *fl)
{
    uint32_t op = in & 0x7f, f3 = (in >> 12) & 7, r1 = (in >> 15) & 31, r2 = (in >> 20) & 31,
             r3 = in >> 27, f7 = in >> 25, fmt = (in >> 25) & 3;
    int d = fmt == 1;
    int rm = f3 == 7 ? (int)s->frm : (int)f3;
    uint64_t a = d ? s->f[r1] : unbox(s->f[r1]);
    uint64_t b = d ? s->f[r2] : unbox(s->f[r2]);
    uint64_t c = d ? s->f[r3] : unbox(s->f[r3]);
    uint64_t r = 0;

    if (fmt > 1)
        return 0; /* only S and D */
    if (op != 0x53) { /* FMADD, FMSUB, FNMSUB, FNMADD */
        if (rm > 4) return 0;
        r = rvfp_fma(d, a, b, c, op == 0x4b || op == 0x4f, op == 0x47 || op == 0x4f, rm, fl);
        *fw = 1;
        *fres = d ? r : box((uint32_t)r);
        return 1;
    }
    switch (f7 >> 2) {
    case 0x00: case 0x01: case 0x02: case 0x03: /* FADD, FSUB, FMUL, FDIV */
        if (rm > 4) return 0;
        r = (f7 >> 2) == 0 ? rvfp_add(d, a, b, rm, fl) : (f7 >> 2) == 1 ? rvfp_sub(d, a, b, rm, fl) :
            (f7 >> 2) == 2 ? rvfp_mul(d, a, b, rm, fl) : rvfp_div(d, a, b, rm, fl);
        break;
    case 0x0b: /* FSQRT */
        if (r2 != 0 || rm > 4) return 0;
        r = rvfp_sqrt(d, a, rm, fl);
        break;
    case 0x04: { /* FSGNJ, FSGNJN, FSGNJX */
        uint64_t sb = 1ull << (d ? 63 : 31);
        if (f3 > 2) return 0;
        r = (a & ~sb) | (f3 == 0 ? b & sb : f3 == 1 ? ~b & sb : (a ^ b) & sb);
        break;
    }
    case 0x05: /* FMIN, FMAX */
        if (f3 > 1) return 0;
        r = rvfp_minmax(d, a, b, f3 == 1, fl);
        break;
    case 0x08: /* FCVT.S.D, FCVT.D.S: rs2 names the source format */
        if (r2 != (uint32_t)!d || rm > 4) return 0;
        r = rvfp_f2f(d, d ? unbox(s->f[r1]) : s->f[r1], rm, fl);
        break;
    case 0x14: /* FLE, FLT, FEQ */
        if (f3 > 2) return 0;
        *xw = 1;
        *xres = (uint32_t)(f3 == 0 ? rvfp_le(d, a, b, fl) : f3 == 1 ? rvfp_lt(d, a, b, fl) : rvfp_eq(d, a, b, fl));
        return 1;
    case 0x18: /* FCVT.W, FCVT.WU */
        if (r2 > 1 || rm > 4) return 0;
        *xw = 1;
        *xres = rvfp_f2i(d, a, r2 == 1, rm, fl);
        return 1;
    case 0x1a: /* FCVT.S.W, FCVT.S.WU, FCVT.D.W, FCVT.D.WU */
        if (r2 > 1 || rm > 4) return 0;
        r = rvfp_i2f(d, s->x[r1], r2 == 1, rm, fl);
        break;
    case 0x1c: /* FMV.X.W (singles only: no FMV.X.D on RV32), FCLASS */
        if (r2 != 0 || !((f3 == 0 && !d) || f3 == 1)) return 0;
        *xw = 1;
        *xres = f3 == 0 ? (uint32_t)s->f[r1] : rvfp_classify(d, a);
        return 1;
    case 0x1e: /* FMV.W.X */
        if (r2 != 0 || f3 != 0 || d) return 0;
        r = s->x[r1];
        break;
    default:
        return 0;
    }
    *fw = 1;
    *fres = d ? r : box((uint32_t)r);
    return 1;
}

void rv_step(rv_iss *s, rv_commit *c)
{
    memset(c, 0, sizeof *c);
    uint32_t pc = s->pc;
    c->pc = pc;
    s->steps++;
    s->mcycle++; /* the model has no notion of time; reads are taken from the DUT */
    if (!in_ram(pc, 4)) {
        fail(s, "fetch", pc);
        return;
    }
    uint32_t in = load(s, pc, 4);
    c->insn = in;
    uint32_t op = in & 0x7f, rd = (in >> 7) & 31, f3 = (in >> 12) & 7, r1 = (in >> 15) & 31,
             r2 = (in >> 20) & 31, f7 = in >> 25;
    uint32_t a = s->x[r1], b = s->x[r2];
    int32_t imm_i = sext(in >> 20, 12);
    int32_t imm_s = sext((in >> 25) << 5 | ((in >> 7) & 31), 12);
    int32_t imm_b = sext(((in >> 31) & 1) << 12 | ((in >> 7) & 1) << 11 | ((in >> 25) & 63) << 5 |
                             ((in >> 8) & 15) << 1, 13);
    int32_t imm_j = sext(((in >> 31) & 1) << 20 | ((in >> 12) & 255) << 12 | ((in >> 20) & 1) << 11 |
                             ((in >> 21) & 1023) << 1, 21);
    uint32_t next = pc + 4, res = 0, fl = 0;
    uint64_t fres = 0;
    int wb = 0, fwb = 0, no_count = 0;

    if ((in & 3) != 3)
        goto illegal;
    switch (op) {
    case 0x37: res = in & 0xfffff000u; wb = 1; break;                   /* LUI */
    case 0x17: res = pc + (in & 0xfffff000u); wb = 1; break;            /* AUIPC */
    case 0x6f:                                                          /* JAL */
        next = pc + (uint32_t)imm_j;
        if (next & 3) { trap(s, c, CAUSE_MISALIGNED_FETCH, next); return; }
        res = pc + 4; wb = 1; break;
    case 0x67:                                                          /* JALR */
        if (f3) goto illegal;
        next = (a + (uint32_t)imm_i) & ~1u;
        if (next & 3) { trap(s, c, CAUSE_MISALIGNED_FETCH, next); return; }
        res = pc + 4; wb = 1; break;
    case 0x63: {                                                        /* branches */
        int t;
        switch (f3) {
        case 0: t = a == b; break;
        case 1: t = a != b; break;
        case 4: t = (int32_t)a < (int32_t)b; break;
        case 5: t = (int32_t)a >= (int32_t)b; break;
        case 6: t = a < b; break;
        case 7: t = a >= b; break;
        default: goto illegal;
        }
        if (t) {
            next = pc + (uint32_t)imm_b;
            if (next & 3) { trap(s, c, CAUSE_MISALIGNED_FETCH, next); return; }
        }
        break;
    }
    case 0x03: {                                                        /* loads */
        uint32_t addr = a + (uint32_t)imm_i;
        int size = (f3 & 3) == 0 ? 1 : (f3 & 3) == 1 ? 2 : 4;
        if (f3 == 3 || f3 > 5) goto illegal;
        if (addr & (uint32_t)(size - 1)) { trap(s, c, CAUSE_MISALIGNED_LOAD, addr); return; }
        res = dload(s, 0, addr, size, 0, NULL);
        if (f3 == 0) res = (uint32_t)sext(res, 8);
        if (f3 == 1) res = (uint32_t)sext(res, 16);
        wb = 1; break;
    }
    case 0x23: {                                                        /* stores */
        uint32_t addr = a + (uint32_t)imm_s;
        int size = 1 << f3;
        if (f3 > 2) goto illegal;
        if (addr & (uint32_t)(size - 1)) { trap(s, c, CAUSE_MISALIGNED_STORE, addr); return; }
        if (s->data_hook && in_ram(addr, (uint32_t)size))
            (void)dload(s, 1, addr, size, b, NULL);
        store(s, c, addr, b, size);
        break;
    }
    case 0x2f: {                                                        /* A: LR, SC, AMOs */
        uint32_t f5 = in >> 27, old, nv;
        int ok = 0, lr = f5 == 2;
        if (f3 != 2 || (lr && r2 != 0)) goto illegal;
        if (f5 != 0 && f5 != 1 && f5 != 2 && f5 != 3 && f5 != 4 && f5 != 8 && f5 != 12 && f5 != 16 &&
            f5 != 20 && f5 != 24 && f5 != 28)
            goto illegal;
        if (a & 3) { trap(s, c, lr ? CAUSE_MISALIGNED_LOAD : CAUSE_MISALIGNED_STORE, a); return; }
        if (!(a & 0x80000000u)) { trap(s, c, lr ? CAUSE_LOAD_FAULT : CAUSE_STORE_FAULT, a); return; }
        if (lr) {
            res = dload(s, 2, a, 4, 0, NULL);
            s->resv_valid = 1;
            s->resv_addr = a & ~15u;
        } else if (f5 == 3) {
            (void)dload(s, 3, a, 4, b, &ok);
            s->resv_valid = 0;
            if (ok)
                store(s, c, a, b, 4);
            res = !ok;
        } else {
            old = dload(s, 4, a, 4, b, NULL);
            switch (f5) {
            case 1: nv = b; break;
            case 0: nv = old + b; break;
            case 4: nv = old ^ b; break;
            case 12: nv = old & b; break;
            case 8: nv = old | b; break;
            case 16: nv = (int32_t)old < (int32_t)b ? old : b; break;
            case 20: nv = (int32_t)old > (int32_t)b ? old : b; break;
            case 24: nv = old < b ? old : b; break;
            default: nv = old > b ? old : b; break;
            }
            store(s, c, a, nv, 4);
            res = old;
        }
        wb = 1; break;
    }
    case 0x07: {                                                        /* FLW, FLD */
        uint32_t addr = a + (uint32_t)imm_i;
        if (!s->has_fpu || s->fs == 0 || (f3 != 2 && f3 != 3)) goto illegal;
        /* FLD is two word accesses: only word alignment is required */
        if (addr & 3) { trap(s, c, CAUSE_MISALIGNED_LOAD, addr); return; }
        fres = f3 == 2 ? box(load(s, addr, 4)) : (uint64_t)load(s, addr + 4, 4) << 32 | load(s, addr, 4);
        fwb = 1; break;
    }
    case 0x27: {                                                        /* FSW, FSD */
        uint32_t addr = a + (uint32_t)imm_s;
        if (!s->has_fpu || s->fs == 0 || (f3 != 2 && f3 != 3)) goto illegal;
        if (addr & 3) { trap(s, c, CAUSE_MISALIGNED_STORE, addr); return; }
        store(s, c, addr, (uint32_t)s->f[r2], 4);
        if (f3 == 3) {
            rv_commit hi;
            memset(&hi, 0, sizeof hi);
            store(s, &hi, addr + 4, (uint32_t)(s->f[r2] >> 32), 4);
            c->mem_dbl = 1;
            c->mem_wdata_hi = hi.mem_wdata;
        }
        break;
    }
    case 0x43: case 0x47: case 0x4b: case 0x4f: case 0x53:              /* F/D computational */
        if (!s->has_fpu || s->fs == 0 || !fp_exec(s, in, &fwb, &fres, &wb, &res, &fl)) goto illegal;
        break;
    case 0x13: {                                                        /* OP-IMM */
        uint32_t sh = r2;
        switch (f3) {
        case 0: res = a + (uint32_t)imm_i; break;
        case 1: if (f7) goto illegal; res = a << sh; break;
        case 2: res = (int32_t)a < imm_i; break;
        case 3: res = a < (uint32_t)imm_i; break;
        case 4: res = a ^ (uint32_t)imm_i; break;
        case 5:
            if (f7 == 0) res = a >> sh;
            else if (f7 == 0x20) res = (uint32_t)((int32_t)a >> sh);
            else goto illegal;
            break;
        case 6: res = a | (uint32_t)imm_i; break;
        default: res = a & (uint32_t)imm_i; break;
        }
        wb = 1; break;
    }
    case 0x33:                                                          /* OP */
        if (f7 == 1) { res = muldiv(f3, a, b); wb = 1; break; }
        if (f7 != 0 && !(f7 == 0x20 && (f3 == 0 || f3 == 5))) goto illegal;
        switch (f3) {
        case 0: res = f7 ? a - b : a + b; break;
        case 1: res = a << (b & 31); break;
        case 2: res = (int32_t)a < (int32_t)b; break;
        case 3: res = a < b; break;
        case 4: res = a ^ b; break;
        case 5: res = f7 ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31); break;
        case 6: res = a | b; break;
        default: res = a & b; break;
        }
        wb = 1; break;
    case 0x0f:                                                          /* FENCE, FENCE.I */
        if (f3 > 1) goto illegal;
        break; /* the model performs every access at once, in program order: both are no-ops */
    case 0x73:                                                          /* SYSTEM */
        if (f3 == 0) {
            if (in == 0x00000073u) { trap(s, c, CAUSE_ECALL_M, 0); return; }
            if (in == 0x00100073u) { trap(s, c, CAUSE_BREAKPOINT, 0); return; }
            if (in == 0x30200073u) {                                    /* MRET */
                next = s->mepc;
                s->mie_bit = s->mpie_bit;
                s->mpie_bit = 1;
                break;
            }
            if (in == 0x10500073u) break;                               /* WFI = nop */
            goto illegal;
        } else if (f3 != 4) {
            uint32_t csr = in >> 20, old = 0, src = (f3 & 4) ? r1 : a;
            int writes = (f3 & 3) == 1 || r1 != 0;
            if (csr_read(s, csr, &old) || (writes && (csr >> 10) == 3))
                goto illegal;
            if (csr_nondet(csr)) {
                c->nondet = 1;
                if (s->have_override)
                    old = s->override_val;
            }
            if (writes) {
                uint32_t nv = (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? old | src : old & ~src;
                no_count = csr_write(s, csr, nv);
            }
            res = old; wb = 1;
            break;
        }
        goto illegal;
    default:
        goto illegal;
    }

    if (wb && rd) {
        s->x[rd] = res;
        c->rd_we = 1;
        c->rd = rd;
        c->rd_val = res;
    }
    if (fwb) {
        s->f[rd] = fres;
        c->frd_we = 1;
        c->frd = rd;
        c->frd_val = fres;
    }
    if (fwb || fl) { /* f register or fflags changed: mstatus.FS = dirty */
        s->fflags |= fl;
        s->fs = 3;
    }
    c->fflags = fl;
    s->pc = next;
    if (!no_count)
        s->minstret++;
    return;

illegal:
    trap(s, c, CAUSE_ILLEGAL, in);
}

/* ---------------------------------------------------------------- trace */

void rv_format_commit(const rv_commit *c, char *buf, size_t n)
{
    char dis[64];
    int k;
    rv_disasm(c->pc, c->insn, dis, sizeof dis);
    k = snprintf(buf, n, "%08x %08x %-28s", c->pc, c->insn, dis);
    if (k < 0 || (size_t)k >= n)
        return;
    if (c->trap)
        k += snprintf(buf + k, n - (size_t)k, " trap cause=%u", c->cause);
    if (c->rd_we && (size_t)k < n)
        k += snprintf(buf + k, n - (size_t)k, " x%-2u=%08x", c->rd, c->rd_val);
    if (c->frd_we && (size_t)k < n)
        k += snprintf(buf + k, n - (size_t)k, " f%-2u=%08x%08x", c->frd, (uint32_t)(c->frd_val >> 32),
                      (uint32_t)c->frd_val);
    if (c->fflags && (size_t)k < n)
        k += snprintf(buf + k, n - (size_t)k, " fflags|=%02x", c->fflags);
    if (c->mem_we && (size_t)k < n)
        k += snprintf(buf + k, n - (size_t)k, " mem[%08x]=%08x/%x", c->mem_addr, c->mem_wdata, c->mem_wmask);
    if (c->mem_dbl && (size_t)k < n)
        snprintf(buf + k, n - (size_t)k, " mem[%08x]=%08x/f", c->mem_addr + 4, c->mem_wdata_hi);
}
