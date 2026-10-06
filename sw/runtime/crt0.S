/* Start-up code for bare-metal C programs: clear the registers, set up the
 * stack, global pointer and trap vector, zero .bss, run main() and pass its
 * return value to exit(). */
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
        la sp, __stack_top
        la t0, rt_trap_entry
        csrw mtvec, t0
#ifdef __riscv_flen
        /* built for a core with the FPU: turn it on (mstatus.FS = initial),
         * round to nearest, no flags */
        li t0, 0x2000
        csrs mstatus, t0
        csrwi fcsr, 0
#endif
        la t0, __bss_start
        la t1, __bss_end
1:      bgeu t0, t1, 2f
        sw zero, 0(t0)
        addi t0, t0, 4
        j 1b
2:      li a0, 0
        li a1, 0
        call main
        call exit

/* Any trap in a C program is a bug: report it and stop with exit code 99. */
        .align 2
rt_trap_entry:
        csrr a0, mcause
        csrr a1, mepc
        csrr a2, mtval
        la sp, __stack_top
        call rt_unexpected_trap
3:      j 3b
