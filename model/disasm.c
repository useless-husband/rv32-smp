/* Small RV32IMFD + Zicsr disassembler for traces and the pipeline viewer. */
#include <stdio.h>

#include "rv_iss.h"

static const char *R[32] = {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",
                            "a1",   "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5",
                            "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};

static int sx(unsigned v, int bits) { return (int)(v << (32 - bits)) >> (32 - bits); }

static const char *csr_name(unsigned a)
{
    switch (a) {
    case 0x001: return "fflags";
    case 0x002: return "frm";
    case 0x003: return "fcsr";
    case 0x300: return "mstatus";
    case 0x301: return "misa";
    case 0x304: return "mie";
    case 0x305: return "mtvec";
    case 0x340: return "mscratch";
    case 0x341: return "mepc";
    case 0x342: return "mcause";
    case 0x343: return "mtval";
    case 0x344: return "mip";
    case 0xB00: return "mcycle";
    case 0xB02: return "minstret";
    case 0xB80: return "mcycleh";
    case 0xB82: return "minstreth";
    case 0xC00: return "cycle";
    case 0xC02: return "instret";
    case 0xC80: return "cycleh";
    case 0xC82: return "instreth";
    case 0xF14: return "mhartid";
    }
    return NULL;
}

/* F and D: returns 1 if `in` was recognised */
static int disasm_fp(uint32_t in, char *b, size_t n)
{
    unsigned op = in & 0x7f, rd = (in >> 7) & 31, f3 = (in >> 12) & 7, r1 = (in >> 15) & 31,
             r2 = (in >> 20) & 31, r3 = in >> 27, f5 = in >> 27;
    char w = ((in >> 25) & 3) ? 'd' : 's';
    int ii = sx(in >> 20, 12);
    int is = sx((in >> 25) << 5 | ((in >> 7) & 31), 12);
    static const char *fma[4] = {"fmadd", "fmsub", "fnmsub", "fnmadd"};
    static const char *ar[4] = {"fadd", "fsub", "fmul", "fdiv"};
    static const char *sj[4] = {"fsgnj", "fsgnjn", "fsgnjx", "fsgnj?"};
    static const char *cm[4] = {"fle", "flt", "feq", "fcmp?"};

    switch (op) {
    case 0x07: snprintf(b, n, "%s f%u, %d(%s)", f3 == 3 ? "fld" : "flw", rd, ii, R[r1]); return f3 == 2 || f3 == 3;
    case 0x27: snprintf(b, n, "%s f%u, %d(%s)", f3 == 3 ? "fsd" : "fsw", r2, is, R[r1]); return f3 == 2 || f3 == 3;
    case 0x43: case 0x47: case 0x4b: case 0x4f:
        snprintf(b, n, "%s.%c f%u, f%u, f%u, f%u", fma[(op >> 2) & 3], w, rd, r1, r2, r3);
        return 1;
    case 0x53:
        switch (f5) {
        case 0x00: case 0x01: case 0x02: case 0x03:
            snprintf(b, n, "%s.%c f%u, f%u, f%u", ar[f5], w, rd, r1, r2); return 1;
        case 0x0b: snprintf(b, n, "fsqrt.%c f%u, f%u", w, rd, r1); return 1;
        case 0x04: snprintf(b, n, "%s.%c f%u, f%u, f%u", sj[f3 & 3], w, rd, r1, r2); return 1;
        case 0x05: snprintf(b, n, "%s.%c f%u, f%u, f%u", f3 ? "fmax" : "fmin", w, rd, r1, r2); return 1;
        case 0x08: snprintf(b, n, "fcvt.%c.%c f%u, f%u", w, w == 'd' ? 's' : 'd', rd, r1); return 1;
        case 0x14: snprintf(b, n, "%s.%c %s, f%u, f%u", cm[f3 & 3], w, R[rd], r1, r2); return 1;
        case 0x18: snprintf(b, n, "fcvt.w%s.%c %s, f%u", r2 ? "u" : "", w, R[rd], r1); return 1;
        case 0x1a: snprintf(b, n, "fcvt.%c.w%s f%u, %s", w, r2 ? "u" : "", rd, R[r1]); return 1;
        case 0x1c:
            if (f3 == 1) snprintf(b, n, "fclass.%c %s, f%u", w, R[rd], r1);
            else snprintf(b, n, "fmv.x.w %s, f%u", R[rd], r1);
            return 1;
        case 0x1e: snprintf(b, n, "fmv.w.x f%u, %s", rd, R[r1]); return 1;
        }
    }
    return 0;
}

void rv_disasm(uint32_t pc, uint32_t in, char *b, size_t n)
{
    unsigned op = in & 0x7f, rd = (in >> 7) & 31, f3 = (in >> 12) & 7, r1 = (in >> 15) & 31,
             r2 = (in >> 20) & 31, f7 = in >> 25;
    int ii = sx(in >> 20, 12);
    int is = sx((in >> 25) << 5 | ((in >> 7) & 31), 12);
    int ib = sx(((in >> 31) & 1) << 12 | ((in >> 7) & 1) << 11 | ((in >> 25) & 63) << 5 | ((in >> 8) & 15) << 1, 13);
    int ij = sx(((in >> 31) & 1) << 20 | ((in >> 12) & 255) << 12 | ((in >> 20) & 1) << 11 | ((in >> 21) & 1023) << 1, 21);
    static const char *br[8] = {"beq", "bne", 0, 0, "blt", "bge", "bltu", "bgeu"};
    static const char *ld[8] = {"lb", "lh", "lw", 0, "lbu", "lhu", 0, 0};
    static const char *st[8] = {"sb", "sh", "sw", 0, 0, 0, 0, 0};
    static const char *oi[8] = {"addi", "slli", "slti", "sltiu", "xori", "srli", "ori", "andi"};
    static const char *oo[8] = {"add", "sll", "slt", "sltu", "xor", "srl", "or", "and"};
    static const char *md[8] = {"mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"};
    static const char *cs[8] = {0, "csrrw", "csrrs", "csrrc", 0, "csrrwi", "csrrsi", "csrrci"};

    if (disasm_fp(in, b, n))
        return;
    switch (op) {
    case 0x37: snprintf(b, n, "lui %s, 0x%x", R[rd], in >> 12); return;
    case 0x17: snprintf(b, n, "auipc %s, 0x%x", R[rd], in >> 12); return;
    case 0x6f: snprintf(b, n, "jal %s, 0x%x", R[rd], pc + (unsigned)ij); return;
    case 0x67: snprintf(b, n, "jalr %s, %d(%s)", R[rd], ii, R[r1]); return;
    case 0x63:
        if (br[f3]) { snprintf(b, n, "%s %s, %s, 0x%x", br[f3], R[r1], R[r2], pc + (unsigned)ib); return; }
        break;
    case 0x03:
        if (ld[f3]) { snprintf(b, n, "%s %s, %d(%s)", ld[f3], R[rd], ii, R[r1]); return; }
        break;
    case 0x23:
        if (st[f3]) { snprintf(b, n, "%s %s, %d(%s)", st[f3], R[r2], is, R[r1]); return; }
        break;
    case 0x13:
        if (f3 == 1 || f3 == 5)
            snprintf(b, n, "%s %s, %s, %u", f3 == 5 && f7 == 0x20 ? "srai" : oi[f3], R[rd], R[r1], r2);
        else if (in == 0x00000013u)
            snprintf(b, n, "nop");
        else
            snprintf(b, n, "%s %s, %s, %d", oi[f3], R[rd], R[r1], ii);
        return;
    case 0x33:
        snprintf(b, n, "%s %s, %s, %s",
                 f7 == 1 ? md[f3] : f7 == 0x20 ? (f3 == 0 ? "sub" : "sra") : oo[f3], R[rd], R[r1], R[r2]);
        return;
    case 0x0f: snprintf(b, n, f3 == 1 ? "fence.i" : "fence"); return;
    case 0x73:
        if (in == 0x00000073u) { snprintf(b, n, "ecall"); return; }
        if (in == 0x00100073u) { snprintf(b, n, "ebreak"); return; }
        if (in == 0x30200073u) { snprintf(b, n, "mret"); return; }
        if (in == 0x10500073u) { snprintf(b, n, "wfi"); return; }
        if (cs[f3]) {
            const char *name = csr_name(in >> 20);
            char cb[16];
            if (!name) { snprintf(cb, sizeof cb, "0x%03x", in >> 20); name = cb; }
            if (f3 & 4) snprintf(b, n, "%s %s, %s, %u", cs[f3], R[rd], name, r1);
            else snprintf(b, n, "%s %s, %s, %s", cs[f3], R[rd], name, R[r1]);
            return;
        }
        break;
    }
    snprintf(b, n, ".word 0x%08x", in);
}
