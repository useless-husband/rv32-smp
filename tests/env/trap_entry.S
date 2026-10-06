/* Trap entry for the riscv-tests environment: save every register in a
 * frame (mscratch points at it), call rvtest_trap() in C on a private stack,
 * restore and return to mepc (which the handler may have advanced). */
        .section .text
        .align 2
        .globl rvtest_trap_entry
rvtest_trap_entry:
        csrrw sp, mscratch, sp          /* sp = frame, mscratch = old sp */
        sw x1, 4(sp)
        .irp n, 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        sw x\n, (\n*4)(sp)
        .endr
        csrr t0, mscratch
        sw t0, 8(sp)                    /* original sp */
        sw zero, 0(sp)                  /* x0 slot reads as zero */
        mv a0, sp
        addi sp, sp, 2047
        addi sp, sp, 2047               /* private stack inside the frame area */
        andi sp, sp, -16
        call rvtest_trap
        la sp, rvtest_trap_frame
        lw t0, 8(sp)
        csrw mscratch, t0               /* swapped back below */
        lw x1, 4(sp)
        .irp n, 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        lw x\n, (\n*4)(sp)
        .endr
        csrrw sp, mscratch, sp          /* sp = original, mscratch = frame */
        mret

        .section .bss
        .align 4
        .globl rvtest_trap_frame
rvtest_trap_frame:
        .space 4096
