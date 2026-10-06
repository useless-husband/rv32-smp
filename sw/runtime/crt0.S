/* Start-up code for bare-metal C programs on every hart.
 *
 * All harts start here after reset.  Each one clears its registers, sets up
 * the global pointer, its own 16 KiB stack (hart k: the k-th block below the
 * top of RAM) and the trap vector.  Hart 0 zeroes .bss, releases the others
 * and runs main(); its return value goes to exit().  The other harts wait for
 * the release and then for work from rt_run_all() (smp.c). */
#include "rv_platform.h"

        .section .text.init
        .globl _start
_start:
        .irp n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        li x\n, 0
        .endr
        .option push
        .option norelax
        la gp, __global_pointer$
        .option pop
        csrr s0, mhartid
        la sp, __stack_top
        slli t0, s0, 14
        sub sp, sp, t0
        la t0, rt_trap_entry
        csrw mtvec, t0
        bnez s0, 3f
        la t0, __bss_start
        la t1, __bss_end
1:      bgeu t0, t1, 2f
        sw zero, 0(t0)
        addi t0, t0, 4
        j 1b
2:      fence rw, rw
        la t0, rt_boot_released
        li t1, 1
        sw t1, 0(t0)
        li a0, 0
        li a1, 0
        call main
        call exit
        /* the other harts: wait until hart 0 has zeroed .bss */
3:      la t0, rt_boot_released
4:      lw t1, 0(t0)
        beqz t1, 4b
        fence rw, rw
        mv a0, s0
        call rt_secondary
5:      j 5b

/* Any trap in a C program is a bug: report it and stop with exit code 99. */
        .align 2
rt_trap_entry:
        csrr a0, mcause
        csrr a1, mepc
        csrr a2, mtval
        csrr a3, mhartid
        la sp, __stack_top
        slli a4, a3, 14
        sub sp, sp, a4
        call rt_unexpected_trap
6:      j 6b
