/* Linker script for programs on this platform: 1 MiB of RAM at 0x80000000,
 * the reset PC.  The stack starts at the top of RAM and grows down. */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
  RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 1M
}

SECTIONS
{
  .text : {
    *(.text.init)
    *(.text .text.*)
  } > RAM
  .rodata : ALIGN(16) {
    *(.rodata .rodata.* .srodata .srodata.*)
  } > RAM
  .data : ALIGN(16) {
    *(.data .data.*)
    __global_pointer$ = . + 0x800;
    *(.sdata .sdata.*)
  } > RAM
  .bss (NOLOAD) : ALIGN(16) {
    __bss_start = .;
    *(.sbss .sbss.* .bss .bss.* COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } > RAM
  _end = .;
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) *(.eh_frame*) }
}
