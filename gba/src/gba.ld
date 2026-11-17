OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
  rom   : ORIGIN = 0x08000000, LENGTH = 32M
  iwram : ORIGIN = 0x03000000, LENGTH = 32K - 0x400
  ewram : ORIGIN = 0x02000000, LENGTH = 256K
}

__sp_irq = 0x03007FA0;
__sp_usr = 0x03007E00;   /* 416 B of IRQ stack: the VBlank interrupt runs the music */

SECTIONS {
  .crt0 : { KEEP(*(.crt0)) } > rom
  .text : { *(EXCLUDE_FILE(*.iwram.o) .text .text.*) *(.romcode) *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx) } > rom
  .rodata : { *(.rodata .rodata.*) . = ALIGN(4); } > rom
  .ARM.exidx : { *(.ARM.exidx*) } > rom
  __iwram_lma = ALIGN(4);
  .iwram : AT(__iwram_lma) {
    __iwram_start = .;
    *(.iwram .iwram.*)
    *.iwram.o(.text .text.*)
    *(.data .data.*)
    . = ALIGN(4);
    __iwram_end = .;
  } > iwram
  .bss (NOLOAD) : { __bss_start = .; *(.bss .bss.*) *(COMMON) . = ALIGN(4); __bss_end = .; } > iwram
  __ewram_lma = __iwram_lma + SIZEOF(.iwram);
  .ewram : AT(__ewram_lma) { __ewram_start = .; *(.ewram .ewram.*) . = ALIGN(4); __ewram_end = .; } > ewram
  .sbss (NOLOAD) : { __sbss_start = .; *(.sbss .sbss.*) . = ALIGN(4); __sbss_end = .; } > ewram
  __rom_end = __ewram_lma + SIZEOF(.ewram);
  end = __sbss_end; _end = __sbss_end;
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
/* the deepest physics call chain (narrow phase) needs about 1.1 KB of stack */
ASSERT(__bss_end + 1280 <= __sp_usr, "IWRAM: not enough room left for the stack")
