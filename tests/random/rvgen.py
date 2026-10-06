#!/usr/bin/env python3
"""Random RV32IM instruction-stream generator for lockstep testing.

The programs are not checked against expected values: the simulator runs
them on a core in lockstep with the golden model, which compares every
committed instruction.  So the generator only has to produce programs that
terminate and that stress the places where pipelines go wrong:

* back-to-back dependencies (sources are drawn mostly from the last few
  destinations), load-use pairs, stores followed by loads of the same word;
* branches and jumps with loads, stores, divides and CSR accesses in their
  shadow (the instructions after a taken branch must be squashed);
* short backward loops (the predictor learns, then mispredicts the exit);
* memory accesses spread at 2 KiB strides, which all land in the same
  D-cache set and force conflict misses and dirty write-backs;
* divide edge cases (0, -1, INT_MIN), counter and CSR reads/writes, traps
  (ECALL, EBREAK, illegal instructions, misaligned loads/stores/jumps; the
  handler skips the instruction), uncached I/O accesses, and self-modifying
  code made visible with FENCE.I.

Register use: x1-x27 random; x28 loop counter; x29 address scratch;
x30 trap-handler scratch; x31 data base pointer.

With --fp the stream also holds F and D instructions (class FpGen below) for
the core with the FPU:

* every arithmetic operation in both formats and all rounding modes (static
  and through frm), on operands drawn from a pool of special values: zeros,
  infinities, quiet and signaling NaNs, subnormals, the largest and smallest
  normals, values one unit in the last place apart (rounding boundaries and
  cancellation), singles that are not NaN-boxed;
* dependencies between consecutive FP instructions, FLW/FLD followed by a
  use, integer results of FP instructions used at once (compare then branch,
  FCVT then arithmetic), integer values converted right after they are made;
* FLD/FSD at addresses 12 mod 16, which straddle two D-cache lines, in the
  eight same-set lines, misaligned ones (trap) and to the I/O region;
* FP instructions in the shadow of branches (they must leave no exception
  flags behind), fflags/frm/fcsr reads and writes in between, invalid
  rounding modes, illegal F/D encodings, and stretches with mstatus.FS
  turned off (every F/D instruction must trap).

Usage: rvgen.py --seed N [--length N] [--fp] -o out.S
"""

import argparse
import random

WORK = list(range(1, 28))
DATA_BYTES = 16384
ALU_RR = ["add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and"]
ALU_RI = ["addi", "slti", "sltiu", "xori", "ori", "andi"]
SHIFT_I = ["slli", "srli", "srai"]
MULDIV = ["mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"]
BRANCH = ["beq", "bne", "blt", "bge", "bltu", "bgeu"]
LOADS = [("lb", 1), ("lbu", 1), ("lh", 2), ("lhu", 2), ("lw", 4)]
STORES = [("sb", 1), ("sh", 2), ("sw", 4)]
SPECIAL = [0, 1, -1, 0x7FFFFFFF, -0x80000000, 0x80000000 - 1, 2, -2, 0xFFFF, 0x8000]


def addi_encoding(rd, rs1, imm):
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (0 << 12) | (rd << 7) | 0x13


class Gen:
    def __init__(self, seed, length):
        self.r = random.Random(seed)
        self.seed = seed
        self.length = length
        self.out = []
        self.recent = []
        self.label = 0
        self.count = 0

    # -- helpers -----------------------------------------------------------
    def emit(self, s):
        self.out.append("    " + s)
        self.count += 1

    def lab(self):
        self.label += 1
        return f"L{self.label}"

    def dst(self, avoid=()):
        while True:
            d = self.r.choice(WORK)
            if d not in avoid:
                break
        self.recent = ([d] + self.recent)[:4]
        return d

    def src(self):
        if self.recent and self.r.random() < 0.6:
            return self.r.choice(self.recent)
        return self.r.choice([0] + WORK)

    def imm12(self):
        return self.r.choice([0, 1, -1, 2047, -2048, self.r.randint(-2048, 2047)])

    # -- instruction groups --------------------------------------------------
    def alu(self):
        k = self.r.random()
        if k < 0.45:
            self.emit(f"{self.r.choice(ALU_RR)} x{self.dst()}, x{self.src()}, x{self.src()}")
        elif k < 0.75:
            self.emit(f"{self.r.choice(ALU_RI)} x{self.dst()}, x{self.src()}, {self.imm12()}")
        elif k < 0.9:
            self.emit(f"{self.r.choice(SHIFT_I)} x{self.dst()}, x{self.src()}, {self.r.randint(0, 31)}")
        elif k < 0.95:
            self.emit(f"lui x{self.dst()}, {self.r.randint(0, 0xFFFFF)}")
        else:
            self.emit(f"auipc x{self.dst()}, {self.r.randint(0, 0xFFFFF)}")

    def muldiv(self):
        if self.r.random() < 0.3:  # edge-case operands
            a, b = self.dst(), self.dst()
            self.emit(f"li x{a}, {self.r.choice(SPECIAL)}")
            self.emit(f"li x{b}, {self.r.choice(SPECIAL)}")
            self.emit(f"{self.r.choice(MULDIV)} x{self.dst()}, x{a}, x{b}")
        else:
            self.emit(f"{self.r.choice(MULDIV)} x{self.dst()}, x{self.src()}, x{self.src()}")

    def base(self):
        """Pick a base register: x31 (middle of the data) or x29 loaded with
        one of eight addresses 2 KiB apart (same D-cache set)."""
        if self.r.random() < 0.5:
            return 31, self.r.randint(-2048, 2040)
        self.emit(f"la x29, data + {self.r.randrange(8) * 2048}")
        return 29, self.r.randint(0, 2040)

    def mem(self):
        b, off = self.base()
        if self.r.random() < 0.04:  # misaligned: traps, the handler skips it
            off |= 1
        else:
            off &= ~3
        if self.r.random() < 0.5:
            op, size = self.r.choice(STORES)
            if off % size and self.r.random() < 0.5:
                off -= off % size
            self.emit(f"{op} x{self.src()}, {off}(x{b})")
            if self.r.random() < 0.3:  # load the same place right after the store
                lop, lsize = self.r.choice(LOADS)
                self.emit(f"{lop} x{self.dst()}, {off - off % lsize}(x{b})")
        else:
            op, size = self.r.choice(LOADS)
            if off % size and self.r.random() < 0.5:
                off -= off % size
            d = self.dst()
            self.emit(f"{op} x{d}, {off}(x{b})")
            if self.r.random() < 0.5:  # load-use
                self.emit(f"{self.r.choice(ALU_RR)} x{self.dst()}, x{d}, x{self.src()}")

    def shadow(self, n):
        for _ in range(n):
            self.r.choice([self.alu, self.alu, self.mem, self.muldiv, self.csr])()

    def branch(self):
        target = self.lab()
        self.emit(f"{self.r.choice(BRANCH)} x{self.src()}, x{self.src()}, {target}")
        self.shadow(self.r.randint(1, 4))
        self.out.append(f"{target}:")

    def jump(self):
        target = self.lab()
        k = self.r.random()
        if k < 0.4:
            self.emit(f"jal x{self.dst()}, {target}")
        elif k < 0.9:
            off = self.r.choice([0, 4, 8, -4])
            self.emit(f"la x29, {target} - {off}")
            self.emit(f"jalr x{self.dst()}, {off}(x29)")
        else:  # misaligned target: traps on the jump, the handler skips it
            self.emit(f"la x29, {target}")
            self.emit(f"jalr x{self.dst()}, 2(x29)")
        self.shadow(self.r.randint(1, 3))
        self.out.append(f"{target}:")

    def loop(self):
        top = self.lab()
        self.emit(f"li x28, {self.r.randint(2, 12)}")
        self.out.append(f"{top}:")
        for _ in range(self.r.randint(2, 6)):
            self.r.choice([self.alu, self.alu, self.mem, self.muldiv])()
        if self.r.random() < 0.5:
            self.branch()
        self.emit("addi x28, x28, -1")
        self.emit(f"bnez x28, {top}")

    def csr(self):
        k = self.r.random()
        if k < 0.3:
            self.emit(f"csrr x{self.dst()}, minstret")
        elif k < 0.4:
            self.emit(f"csrr x{self.dst()}, {self.r.choice(['mcycle', 'cycle', 'mhpmcounter5', 'instreth'])}")
        elif k < 0.8:
            op = self.r.choice(["csrrw", "csrrs", "csrrc"])
            # mepc/mcause/mtval are free to use: the trap handler rewrites them
            csr = self.r.choice(["mscratch", "mscratch", "mepc", "mcause", "mtval"])
            self.emit(f"{op} x{self.dst()}, {csr}, x{self.src()}")
        else:
            op = self.r.choice(["csrrwi", "csrrsi", "csrrci"])
            self.emit(f"{op} x{self.dst()}, mscratch, {self.r.randint(0, 31)}")

    def trap(self):
        self.emit(self.r.choice(["ecall", "ebreak", ".word 0x00000000", ".word 0xffffffff",
                                 "csrw cycle, x1", "csrr x5, 0x7c0", ".word 0x02000033 | (1 << 30)",
                                 ".word 0x00002063  # branch with reserved funct3",
                                 ".word 0x00003003  # LD (RV64 only)"]))

    def io(self):
        self.emit(f"li x29, {0x10000000}")
        if self.r.random() < 0.8:
            self.emit(f"sb x{self.src()}, 0(x29)")
        else:
            self.emit(f"lw x{self.dst()}, 0(x29)")

    def selfmod(self):
        target = self.lab()
        rd = self.dst()
        enc = addi_encoding(rd, self.src(), self.r.randint(-2048, 2047))
        self.emit(f"la x29, {target}")
        self.emit(f"li x30, {enc}")
        self.emit("sw x30, 0(x29)")
        self.emit("fence.i")
        self.out.append(f"{target}:")
        self.emit("nop  # replaced at run time")

    # -- hooks for FpGen -------------------------------------------------------
    def groups(self):
        return [(self.alu, 30), (self.mem, 22), (self.muldiv, 6), (self.branch, 10), (self.jump, 5),
                (self.loop, 4), (self.csr, 5), (self.trap, 2), (self.io, 2), (self.selfmod, 1)]

    def prologue(self):
        pass

    def data_extra(self):
        pass

    # -- program ---------------------------------------------------------------
    def program(self):
        self.out += [
            f"# generated by tests/random/rvgen.py --seed {self.seed} --length {self.length}",
            '#include "rv_platform.h"',
            "    .section .text.init",
            "    .globl _start",
            "_start:",
            "    la x30, trap_handler",
            "    csrw mtvec, x30",
            "    la x31, data + 8192",
        ]
        for reg in WORK:
            self.emit(f"li x{reg}, {self.r.choice(SPECIAL + [self.r.getrandbits(32)] * 4)}")
        self.prologue()
        groups = self.groups()
        funcs = [g for g, _ in groups]
        weights = [w for _, w in groups]
        while self.count < self.length:
            self.r.choices(funcs, weights)[0]()
        self.out += [
            "    li x29, RV_MMIO_EXIT",
            "    li x30, 1",
            "    sw x30, 0(x29)",
            "1:  j 1b",
            "",
            "    .align 2",
            "trap_handler:",
            "    csrr x30, mcause",
            "    csrr x30, mtval",
            "    csrr x30, mepc",
            "    addi x30, x30, 4",
            "    csrw mepc, x30",
            "    mret",
            "",
            "    .data",
            "    .align 4",
            "data:",
        ]
        for i in range(0, DATA_BYTES // 4, 8):
            words = ", ".join(f"0x{self.r.getrandbits(32):08x}" for _ in range(8))
            self.out.append(f"    .word {words}")
        self.data_extra()
        return "\n".join(self.out) + "\n"


# ---------------------------------------------------------------- F and D
RM = ["rne", "rtz", "rdn", "rup", "rmm", "dyn", "dyn"]
FP_BIN = ["fadd", "fsub", "fmul", "fdiv"]
FP_FMA = ["fmadd", "fmsub", "fnmsub", "fnmadd"]
FP_NORM = ["fsgnj", "fsgnjn", "fsgnjx", "fmin", "fmax"]
FP_CMP = ["feq", "flt", "fle"]
# doubles: zeros, infinities, NaNs (quiet, signaling, with payload), subnormals,
# smallest/largest normals, values around 1 and 2 one ulp apart, powers of two
# at the edges of the integer range, a few ordinary numbers
D_SPECIAL = [
    0x0000000000000000, 0x8000000000000000, 0x7FF0000000000000, 0xFFF0000000000000,
    0x7FF8000000000000, 0xFFF8000000000001, 0x7FF0000000000001, 0x7FF4000000000000,
    0x0000000000000001, 0x800FFFFFFFFFFFFF, 0x0008000000000000, 0x0010000000000000,
    0x0010000000000001, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF, 0x7FE0000000000000,
    0x3FF0000000000000, 0x3FF0000000000001, 0x3FEFFFFFFFFFFFFF, 0xBFF0000000000000,
    0x4000000000000000, 0x3FFFFFFFFFFFFFFF, 0x3FE0000000000000, 0x3FD5555555555555,
    0x3FB999999999999A, 0x4008000000000000, 0x41DFFFFFFFC00000, 0x41E0000000000000,
    0xC1E0000000000000, 0xC1E0000000200000, 0x41EFFFFFFFE00000, 0x41F0000000000000,
    0x4340000000000000, 0x433FFFFFFFFFFFFF, 0x3CA0000000000000, 0x3C90000000000000,
    0x3FE0000000000001, 0x3FDFFFFFFFFFFFFF, 0xBFE0000000000000, 0x3FF8000000000000,
    0x1FF0000000000000, 0x5FF0000000000000, 0x2000000000000000, 0x4024000000000000,
]
S_SPECIAL = [
    0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00001, 0x7F800001, 0x7FA00000,
    0x00000001, 0x807FFFFF, 0x00400000, 0x00800000, 0x00800001, 0x7F7FFFFF, 0xFF7FFFFF, 0x3F800000,
    0x3F800001, 0x3F7FFFFF, 0xBF800000, 0x40000000, 0x3F000000, 0x3EAAAAAB, 0x3DCCCCCD, 0x40400000,
    0x4EFFFFFF, 0x4F000000, 0xCF000000, 0xCF000001, 0x4F7FFFFF, 0x4F800000, 0x4B800000, 0x4B7FFFFF,
    0x33800000, 0x33000000, 0x3F000001, 0x3EFFFFFF, 0x1F800000, 0x5F800000, 0x41200000, 0x3FC00000,
]
POOL_D, POOL_S, POOL_X = 80, 40, 8   # pool entries: doubles, boxed singles, singles not NaN-boxed
POOL = POOL_D + POOL_S + POOL_X


class FpGen(Gen):
    def __init__(self, seed, length):
        super().__init__(seed, length)
        self.frecent = []
        self.ffmt = ["d"] * 32   # what each f register holds (as far as the generator knows)
        self.fs_off = 0          # > 0: FS is off for this many more groups

    def fdst(self, w="d"):
        d = self.r.randrange(32)
        self.frecent = ([d] + self.frecent)[:4]
        self.ffmt[d] = w
        return d

    def fsrc(self, w=None):
        """A source register: usually a recent destination or a register that
        holds the wanted format (a value of the other format reads as a NaN,
        which is worth testing but not most of the time)."""
        k = self.r.random()
        if w is None or k < 0.12:
            return self.r.choice(self.frecent) if self.frecent and k < 0.06 else self.r.randrange(32)
        recent = [f for f in self.frecent if self.ffmt[f] == w]
        if recent and k < 0.55:
            return self.r.choice(recent)
        same = [f for f in range(32) if self.ffmt[f] == w]
        return self.r.choice(same) if same else self.r.randrange(32)

    def w(self):
        return self.r.choice(["s", "d", "d"])

    def pool_index(self, w):
        k = self.r.random()
        if k < 0.04:
            return POOL_D + POOL_S + self.r.randrange(POOL_X)
        return self.r.randrange(POOL_D) if w == "d" else POOL_D + self.r.randrange(POOL_S)

    def pool_load(self, w="d"):
        """Load an f register from the pool with a value of format w."""
        reg = self.fdst(w)
        self.emit(f"la x29, fpool + {8 * self.pool_index(w)}")
        self.emit(f"fld f{reg}, 0(x29)")
        return reg

    def fp_arith(self):
        k = self.r.random()
        w, rm = self.w(), self.r.choice(RM)
        if self.r.random() < 0.3:
            self.pool_load(w)
        if k < 0.45:
            a, b = self.fsrc(w), self.fsrc(w)
            self.emit(f"{self.r.choice(FP_BIN)}.{w} f{self.fdst(w)}, f{a}, f{b}, {rm}")
        elif k < 0.65:
            a, b, c = self.fsrc(w), self.fsrc(w), self.fsrc(w)
            self.emit(f"{self.r.choice(FP_FMA)}.{w} f{self.fdst(w)}, f{a}, f{b}, f{c}, {rm}")
        elif k < 0.75:
            a = self.fsrc(w)
            if self.r.random() < 0.6:  # |x| first, so the root is usually a number
                t = self.fdst(w)
                self.emit(f"fsgnjx.{w} f{t}, f{a}, f{a}")
                a = t
            self.emit(f"fsqrt.{w} f{self.fdst(w)}, f{a}, {rm}")
        elif k < 0.9:
            a, b = self.fsrc(w), self.fsrc(w)
            self.emit(f"{self.r.choice(FP_NORM)}.{w} f{self.fdst(w)}, f{a}, f{b}")
        else:  # the same value on both sides: exact cancellation, x*x, x/x
            a = self.fsrc(w)
            self.emit(f"{self.r.choice(FP_BIN)}.{w} f{self.fdst(w)}, f{a}, f{a}, {rm}")

    def fp_cmp(self):
        d = self.dst()
        w = self.w()
        self.emit(f"{self.r.choice(FP_CMP)}.{w} x{d}, f{self.fsrc(w)}, f{self.fsrc(w)}")
        k = self.r.random()
        if k < 0.4:  # the integer result decides a branch at once
            target = self.lab()
            self.emit(f"{self.r.choice(['beqz', 'bnez'])} x{d}, {target}")
            self.shadow(self.r.randint(1, 3))
            self.out.append(f"{target}:")
        elif k < 0.7:
            self.emit(f"{self.r.choice(ALU_RR)} x{self.dst()}, x{d}, x{self.src()}")

    def fp_cvt(self):
        k = self.r.random()
        w, rm = self.w(), self.r.choice(RM)
        if k < 0.3:  # float -> integer, used at once
            d = self.dst()
            self.emit(f"fcvt.{self.r.choice(['w', 'wu'])}.{w} x{d}, f{self.fsrc(w)}, {rm}")
            if self.r.random() < 0.5:
                self.emit(f"{self.r.choice(ALU_RR)} x{self.dst()}, x{d}, x{self.src()}")
        elif k < 0.55:  # integer -> float, the integer made just before
            if self.r.random() < 0.5:
                self.alu()
            f = self.fdst(w)
            self.emit(f"fcvt.{w}.{self.r.choice(['w', 'wu'])} f{f}, x{self.src()}, {rm}")
            if self.r.random() < 0.5:
                self.emit(f"fadd.{w} f{self.fdst(w)}, f{f}, f{self.fsrc(w)}, {rm}")
        elif k < 0.7:
            if w == "s":
                self.emit(f"fcvt.s.d f{self.fdst('s')}, f{self.fsrc('d')}, {rm}")
            else:
                self.emit(f"fcvt.d.s f{self.fdst('d')}, f{self.fsrc('s')}, {rm}")
        elif k < 0.8:
            self.emit(f"fclass.{w} x{self.dst()}, f{self.fsrc(w)}")
        elif k < 0.9:
            self.emit(f"fmv.x.w x{self.dst()}, f{self.fsrc()}")
        else:
            self.emit(f"fmv.w.x f{self.fdst('s')}, x{self.src()}")

    def fp_mem(self):
        k = self.r.random()
        if k < 0.25:
            w = self.w()
            f = self.pool_load(w)
            if self.r.random() < 0.6:  # load-use
                self.emit(f"{self.r.choice(FP_BIN)}.{w} f{self.fdst(w)}, f{f}, f{self.fsrc(w)}")
            return
        b, off = self.base()
        k2 = self.r.random()
        if k2 < 0.04:
            off |= self.r.choice([1, 2, 3])       # misaligned: traps
        elif k2 < 0.4 and b == 29:
            off = (off & ~15) | 12                # 8 bytes across two cache lines
        elif k2 < 0.6:
            off = (off & ~7) | 4                  # word aligned only
        else:
            off &= ~7
        if off > 2032:                            # keep off + 4 (the upper word) encodable
            off -= 16
        dbl = self.r.random() < 0.7
        w = "d" if dbl else "s"
        if self.r.random() < 0.5:
            self.emit(f"{'fsd' if dbl else 'fsw'} f{self.fsrc(w)}, {off}(x{b})")
            k3 = self.r.random()
            if k3 < 0.3:    # read it back, usually in the same width
                back = dbl if self.r.random() < 0.8 else not dbl
                self.emit(f"{'fld' if back else 'flw'} f{self.fdst('d' if back else 's')}, {off}(x{b})")
            elif k3 < 0.5:  # ... or one of its words as an integer
                self.emit(f"lw x{self.dst()}, {(off & ~3) + self.r.choice([0, 4])}(x{b})")
        else:
            if self.r.random() < 0.3:  # an integer store into the double about to be loaded
                self.emit(f"sw x{self.src()}, {(off & ~3) + self.r.choice([0, 4])}(x{b})")
            f = self.fdst(w)
            self.emit(f"{'fld' if dbl else 'flw'} f{f}, {off}(x{b})")
            if self.r.random() < 0.6:  # load-use, also as the third operand
                a, c = self.fsrc(w), self.fsrc(w)
                self.emit(f"fmadd.{w} f{self.fdst(w)}, f{a}, f{c}, f{f}, {self.r.choice(RM)}")

    def fp_csr(self):
        k = self.r.random()
        if k < 0.35:
            self.emit(f"csrr x{self.dst()}, {self.r.choice(['fflags', 'fflags', 'fcsr', 'frm', 'mstatus'])}")
        elif k < 0.5:
            self.emit(f"csrrw x{self.dst()}, fflags, x{self.src()}")
        elif k < 0.6:
            self.emit(f"csrrci x{self.dst()}, fflags, {self.r.randint(0, 31)}")
        elif k < 0.85:  # a rounding mode; 5-7 are invalid and make dynamic-mode instructions trap
            self.emit(f"csrwi frm, {self.r.choice([0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 5, 7])}")
        elif k < 0.95:
            self.emit(f"csrrw x{self.dst()}, fcsr, x{self.src()}")
        else:
            self.emit(f"csrrs x{self.dst()}, fcsr, x0")

    def fp_trap(self):
        self.emit(self.r.choice([
            ".word 0x0020d053  # fadd.s with reserved rounding mode 5",
            ".word 0x0220e053  # fadd.d with reserved rounding mode 6",
            ".word 0x04208053  # fadd.h (no Zfh)",
            ".word 0x06208053  # fadd.q (no Q)",
            ".word 0xe2008053  # fmv.x.d (RV64 only)",
            ".word 0xf2008053  # fmv.d.x (RV64 only)",
            ".word 0xc0208053  # fcvt.l.s (RV64 only)",
            ".word 0x58108053  # fsqrt.s with rs2 != 0",
            ".word 0x20003053  # fsgnj.s with funct3 = 3",
            ".word 0xa0003053  # compare with funct3 = 3",
            ".word 0x40008053  # fcvt.s.s",
            ".word 0x0000c007  # FP load with funct3 = 4",
            ".word 0x00001027  # FP store with funct3 = 1",
            ".word 0x04000043  # fmadd.h",
        ]))

    def fs_toggle(self):
        """Turn mstatus.FS off for a few groups: every F/D instruction and
        fcsr access traps (and is skipped) until it is turned on again."""
        self.emit("li x29, 0x6000")
        self.emit("csrc mstatus, x29")
        self.fs_off = self.r.randint(2, 6)

    def fs_on(self):
        self.emit(f"li x29, {self.r.choice([0x2000, 0x4000, 0x6000])}")
        self.emit("csrs mstatus, x29")
        if self.r.random() < 0.6:  # a write to an f register alone must make FS dirty
            f = self.r.randrange(32)
            self.emit(f"fsgnj.d f{f}, f{f}, f{f}")
            self.emit(f"csrr x{self.dst()}, mstatus")

    def selfmod(self):
        """Half of the time the replaced instruction is a divide or square
        root: it is in EX, started, when FENCE.I flushes it, and what runs
        instead is a different F/D instruction."""
        if self.r.random() < 0.5:
            return super().selfmod()
        target = self.lab()
        rd, rs1, rs2 = self.fdst("d"), self.fsrc("d"), self.fsrc("d")
        enc = (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (self.r.choice([0, 1, 2, 3, 4]) << 12) | (rd << 7) | 0x53
        enc |= self.r.choice([0, 1, 2]) << 27       # fadd.d / fsub.d / fmul.d
        self.emit(f"la x29, {target}")
        self.emit(f"li x30, {enc}")
        self.emit("sw x30, 0(x29)")
        self.emit("fence.i")
        self.out.append(f"{target}:")
        if self.r.random() < 0.5:
            self.emit(f"fdiv.d f{self.r.randrange(32)}, f{self.fsrc('d')}, f{self.fsrc('d')}  # replaced at run time")
        else:
            self.emit(f"fsqrt.d f{self.r.randrange(32)}, f{self.fsrc('d')}  # replaced at run time")

    def shadow(self, n):
        for _ in range(n):
            self.r.choice([self.alu, self.mem, self.muldiv, self.csr, self.fp_arith, self.fp_arith,
                           self.fp_cvt, self.fp_mem, self.fp_csr])()

    def loop(self):
        top = self.lab()
        self.emit(f"li x28, {self.r.randint(2, 8)}")
        self.out.append(f"{top}:")
        for _ in range(self.r.randint(2, 6)):
            self.r.choice([self.alu, self.mem, self.fp_arith, self.fp_arith, self.fp_mem, self.fp_cvt])()
        if self.r.random() < 0.5:
            self.branch()
        self.emit("addi x28, x28, -1")
        self.emit(f"bnez x28, {top}")

    def step(self, f):
        def run():
            f()
            if self.fs_off:
                self.fs_off -= 1
                if not self.fs_off:
                    self.fs_on()
        return run

    def groups(self):
        g = [(self.alu, 14), (self.mem, 8), (self.muldiv, 2), (self.branch, 8), (self.jump, 3), (self.loop, 4),
             (self.csr, 2), (self.trap, 1), (self.io, 1), (self.selfmod, 1),
             (self.fp_arith, 26), (self.fp_mem, 14), (self.fp_cmp, 6), (self.fp_cvt, 8), (self.fp_csr, 4),
             (self.fp_trap, 1), (self.fs_toggle, 0.5)]
        return [(self.step(f), w) for f, w in g]

    def io(self):
        if self.r.random() < 0.5:
            return super().io()
        # an FSD/FSW to the I/O region (two bus writes for the double), FLD reads zeros
        self.emit(f"li x29, {0x10000100 + 8 * self.r.randrange(8)}")
        self.emit(self.r.choice([f"fsd f{self.fsrc()}, 0(x29)", f"fsw f{self.fsrc()}, 4(x29)",
                                 f"fld f{self.fdst()}, 0(x29)"]))

    def prologue(self):
        self.emit("li x29, 0x2000")
        self.emit("csrs mstatus, x29          # FS = initial: FPU on")
        self.emit("la x29, fpool")
        for reg in range(32):
            self.ffmt[reg] = self.r.choice(["s", "d", "d"])
            self.emit(f"fld f{reg}, {8 * self.pool_index(self.ffmt[reg])}(x29)")

    def data_extra(self):
        self.out += ["    .align 4", "fpool:"]
        for i in range(POOL):
            k = self.r.random()
            sign = self.r.getrandbits(1)
            if i < POOL_D:
                if k < 0.35:
                    v = self.r.choice(D_SPECIAL)
                elif k < 0.7:   # near 1: sums and differences that cancel or round
                    v = sign << 63 | (0x3FF + self.r.randint(-3, 3)) << 52 | \
                        self.r.choice([0, 1, (1 << 52) - 1, self.r.getrandbits(52), self.r.getrandbits(52)])
                elif k < 0.85:  # near the bottom and the top of the exponent range
                    v = sign << 63 | self.r.choice([0, 1, 2, 53, 0x7FD, 0x7FE]) << 52 | self.r.getrandbits(52)
                else:
                    v = self.r.getrandbits(64)
            elif i < POOL_D + POOL_S:
                if k < 0.35:
                    lo = self.r.choice(S_SPECIAL)
                elif k < 0.7:
                    lo = sign << 31 | (0x7F + self.r.randint(-3, 3)) << 23 | \
                         self.r.choice([0, 1, (1 << 23) - 1, self.r.getrandbits(23), self.r.getrandbits(23)])
                elif k < 0.85:
                    lo = sign << 31 | self.r.choice([0, 1, 2, 24, 0xFD, 0xFE]) << 23 | self.r.getrandbits(23)
                else:
                    lo = self.r.getrandbits(32)
                v = 0xFFFFFFFF00000000 | lo
            else:               # a single that is not NaN-boxed
                v = self.r.getrandbits(31) << 32 | self.r.choice(S_SPECIAL)
            self.out.append(f"    .word 0x{v & 0xFFFFFFFF:08x}, 0x{v >> 32:08x}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--length", type=int, default=3000, help="approximate instruction count")
    ap.add_argument("--fp", action="store_true", help="include F and D instructions (core with the FPU)")
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()
    with open(a.output, "w") as f:
        f.write((FpGen if a.fp else Gen)(a.seed, a.length).program())


if __name__ == "__main__":
    main()
