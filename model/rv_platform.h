/* Platform constants shared by the golden model, the simulation harness and
 * the bare-metal runtime.  The RTL repeats the same numbers in
 * rtl/rv_defs.svh; tests/system/test_platform.py checks the two agree. */
#ifndef RV_PLATFORM_H
#define RV_PLATFORM_H

/* usable from C and from assembly (the assembler has no 'u' suffix) */
#ifdef __ASSEMBLER__
#define RV_U(x) x
#else
#define RV_U(x) x##u
#endif

#define RV_RESET_PC   RV_U(0x80000000)  /* first instruction fetched after reset */
#define RV_RAM_BASE   RV_U(0x80000000)  /* cacheable RAM: every address with bit 31 set */
#define RV_RAM_SIZE   RV_U(0x00100000)  /* 1 MiB */

/* Uncached I/O (bit 31 clear).  Writes only; reads return 0. */
#define RV_MMIO_CONSOLE RV_U(0x10000000) /* store: low byte goes to the console */
#define RV_MMIO_EXIT    RV_U(0x10000004) /* store: stop the simulation (HTIF style:
                                       1 = pass, (n << 1) | 1 = exit code n) */
#define RV_MMIO_BASE    RV_U(0x10000000)
#define RV_MMIO_SIZE    RV_U(0x00001000)

/* Machine-mode hardware performance counters mhpmcounter3..12.  Each is
 * hard-wired to one event (there are no mhpmevent selectors). */
#define RV_HPM_FIRST 3
#define RV_HPM_COUNT 10
#ifndef __ASSEMBLER__
#define RV_HPM_NAMES {                                   \
    "icache_misses", "dcache_accesses", "dcache_misses", \
    "dcache_writebacks", "branches", "branch_mispredicts", \
    "jumps", "jump_mispredicts", "load_use_stalls", "icache_accesses" }
#endif

#endif
