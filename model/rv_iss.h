/* rv_iss: the golden model.  An instruction-set simulator for RV32IM +
 * Zicsr + Zifencei, optionally with F and D (has_fpu), machine mode only, written to the RISC-V
 * specifications and nothing else.  Both RTL cores are checked against it,
 * instruction by instruction.
 *
 * One call to rv_step() executes one instruction (or takes one exception) and
 * describes its architectural effect in an rv_commit record, the same record
 * the RTL cores emit on their commit port. */
#ifndef RV_ISS_H
#define RV_ISS_H

#include <stddef.h>
#include <stdint.h>

#include "rv_platform.h"

typedef struct {
    uint32_t pc, insn;
    int trap;           /* 1: the instruction raised an exception instead of retiring */
    uint32_t cause;     /* mcause when trap */
    int rd_we;          /* 1: wrote a nonzero rd */
    uint32_t rd, rd_val;
    int mem_we;         /* 1: wrote memory (RAM or I/O) */
    uint32_t mem_addr;  /* word-aligned address */
    uint32_t mem_wdata; /* data in its byte lanes */
    uint32_t mem_wmask; /* 4-bit byte-lane mask */
    int nondet;         /* 1: rd_val came from a cycle/hpm counter read */
    /* F/D (all zero on a model without has_fpu) */
    int frd_we;         /* 1: wrote floating-point register frd */
    uint32_t frd;
    uint64_t frd_val;   /* the whole 64-bit register (singles are NaN-boxed) */
    uint32_t fflags;    /* exception flags this instruction raised */
    int mem_dbl;        /* 1: FSD, an 8-byte write: mem_wdata_hi goes to mem_addr + 4 */
    uint32_t mem_wdata_hi;
} rv_commit;

typedef struct {
    uint32_t pc;
    uint32_t x[32];
    uint8_t *ram;
    /* machine-mode CSRs */
    uint32_t mie_bit, mpie_bit;
    uint32_t mtvec, mscratch, mepc, mcause, mtval;
    uint64_t mcycle, minstret;
    uint64_t hpm[RV_HPM_COUNT];
    /* F and D extensions: set has_fpu after rv_iss_init() to enable them */
    int has_fpu;
    uint64_t f[32];
    uint32_t frm, fflags;
    uint32_t fs;        /* mstatus.FS: 0 off, 1 initial, 2 clean, 3 dirty */
    /* I/O */
    char *console;
    size_t console_len, console_cap;
    int echo;           /* write console bytes to stdout as they appear */
    int exited;
    uint32_t exit_value;
    /* fatal model error (access outside RAM and I/O) */
    int error;
    char errmsg[160];
    /* when set, the next read of a non-deterministic CSR returns this value */
    int have_override;
    uint32_t override_val;
    uint64_t steps;
} rv_iss;

int  rv_iss_init(rv_iss *s);
void rv_iss_free(rv_iss *s);
/* Load an ELF32 RISC-V executable into RAM.  Returns 0 or -1 (message in errmsg). */
int  rv_iss_load_elf(rv_iss *s, const char *path);
/* Write RAM contents as a $readmemh image (word per line, word addresses). */
int  rv_iss_write_hex(const rv_iss *s, const char *path);
void rv_step(rv_iss *s, rv_commit *c);
/* exit code of the program: 0 = pass, n = exit(n) / failing test number */
int  rv_exit_code(uint32_t exit_value);

/* Disassemble one instruction (pc is used for branch and jump targets). */
void rv_disasm(uint32_t pc, uint32_t insn, char *buf, size_t n);
/* One trace line for a commit record (no newline). */
void rv_format_commit(const rv_commit *c, char *buf, size_t n);

#endif
