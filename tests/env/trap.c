/* C half of the riscv-tests trap handler: emulate misaligned loads and
 * stores (mcause 4 and 6) byte by byte, fail the test on anything else. */
#include <stdint.h>

#include "rv_platform.h"

#define CSR_READ(name) ({ uint32_t v_; __asm__ volatile("csrr %0, " #name : "=r"(v_)); v_; })

static void put(const char *s)
{
    while (*s)
        *(volatile uint32_t *)RV_MMIO_CONSOLE = (uint8_t)*s++;
}

static void puthex(uint32_t v)
{
    char b[11] = "0x";
    for (int i = 0; i < 8; i++)
        b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0;
    put(b);
}

void rvtest_trap(uint32_t *x)
{
    uint32_t cause = CSR_READ(mcause), epc = CSR_READ(mepc), addr = CSR_READ(mtval);
    uint32_t insn = *(volatile uint32_t *)epc;
    uint32_t f3 = (insn >> 12) & 7, rd = (insn >> 7) & 31, rs2 = (insn >> 20) & 31;
    int size = 1 << (f3 & 3);
    volatile uint8_t *p = (volatile uint8_t *)addr;

    if (cause == 4 && (insn & 0x7f) == 0x03) {          /* misaligned load */
        uint32_t v = 0;
        for (int i = 0; i < size; i++)
            v |= (uint32_t)p[i] << (8 * i);
        if (f3 == 0) v = (uint32_t)(int32_t)(int8_t)v;
        if (f3 == 1) v = (uint32_t)(int32_t)(int16_t)v;
        if (rd)
            x[rd] = v;
    } else if (cause == 6 && (insn & 0x7f) == 0x23) {   /* misaligned store */
        for (int i = 0; i < size; i++)
            p[i] = (uint8_t)(x[rs2] >> (8 * i));
    } else {
        put("unexpected trap: mcause=");
        puthex(cause);
        put(" mepc=");
        puthex(epc);
        put("\n");
        *(volatile uint32_t *)RV_MMIO_EXIT = (1337u << 1) | 1;
        for (;;)
            ;
    }
    __asm__ volatile("csrw mepc, %0" ::"r"(epc + 4));
}
