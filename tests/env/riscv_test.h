/* Test environment for the official riscv-tests (isa/rv32ui, isa/rv32um,
 * isa/rv32uf, isa/rv32ud) on this platform.  The tests themselves are fetched unmodified at build time;
 * riscv-tests expects every target to supply its own riscv_test.h, and this
 * is ours (machine mode only, results reported through the EXIT register).
 *
 * Misaligned loads and stores raise an exception on these cores (allowed by
 * the ISA).  Like OpenSBI does on real hardware, the trap handler in trap.c
 * emulates them byte by byte, which is what the ma_data test needs.  Any
 * other trap is a failure. */
#ifndef RV32_PIPELINE_RISCV_TEST_H
#define RV32_PIPELINE_RISCV_TEST_H

#include "rv_platform.h"

#define RVTEST_RV64U .macro init; .endm
#define RVTEST_RV32U .macro init; .endm
/* F/D tests: turn the FPU on (mstatus.FS = initial) and clear fcsr */
#define RVTEST_RV32UF .macro init; RVTEST_FP_ENABLE; .endm
#define RVTEST_RV64UF RVTEST_RV32UF
#define RVTEST_FP_ENABLE                                                \
        li a0, 0x2000;                                                  \
        csrs mstatus, a0;                                               \
        csrwi fcsr, 0
#define TESTNUM gp

#define RVTEST_CODE_BEGIN                                               \
        .section .text.init;                                            \
        .align 6;                                                       \
        .globl _start;                                                  \
_start:                                                                 \
        INIT_XREG;                                                      \
        la t0, rvtest_trap_entry;                                       \
        csrw mtvec, t0;                                                 \
        la t0, rvtest_trap_frame;                                       \
        csrw mscratch, t0;                                              \
        li TESTNUM, 0;                                                  \
        init;                                                           \
        j 1f;                                                           \
1:

#define INIT_XREG                                                       \
  li x1, 0;  li x2, 0;  li x3, 0;  li x4, 0;  li x5, 0;  li x6, 0;      \
  li x7, 0;  li x8, 0;  li x9, 0;  li x10, 0; li x11, 0; li x12, 0;     \
  li x13, 0; li x14, 0; li x15, 0; li x16, 0; li x17, 0; li x18, 0;     \
  li x19, 0; li x20, 0; li x21, 0; li x22, 0; li x23, 0; li x24, 0;     \
  li x25, 0; li x26, 0; li x27, 0; li x28, 0; li x29, 0; li x30, 0;     \
  li x31, 0;

#define RVTEST_CODE_END unimp

#define RVTEST_PASS                                                     \
        fence;                                                          \
        li TESTNUM, 1;                                                  \
        li t0, RV_MMIO_EXIT;                                            \
        sw TESTNUM, 0(t0);                                              \
1:      j 1b;

#define RVTEST_FAIL                                                     \
        fence;                                                          \
1:      beqz TESTNUM, 1b;                                               \
        sll TESTNUM, TESTNUM, 1;                                        \
        or TESTNUM, TESTNUM, 1;                                         \
        li t0, RV_MMIO_EXIT;                                            \
        sw TESTNUM, 0(t0);                                              \
2:      j 2b;

#define EXTRA_DATA

#define RVTEST_DATA_BEGIN                                               \
        EXTRA_DATA                                                      \
        .align 4; .global begin_signature; begin_signature:

#define RVTEST_DATA_END .align 4; .global end_signature; end_signature:

#endif
