@ STAK! GBA startup code. Header fields are fixed up by tools/gbafix.py
@ (boot logo, fixed byte, checksum), or devkitPro's gbafix.
    .section .crt0, "ax"
    .global _start
    .arm
    .align 2
_start:
    b       start_vector
    .fill   156, 1, 0           @ boot logo (filled in by gbafix)
    .ascii  "STAK GBA\0\0\0\0"  @ title (12)
    .ascii  "ASTE"              @ game code
    .ascii  "GG"                @ maker code
    .byte   0x96, 0, 0
    .fill   7, 1, 0
    .byte   0                   @ version
    .byte   0                   @ header checksum (gbafix)
    .fill   2, 1, 0

start_vector:
    mov     r0, #0x12           @ IRQ mode
    msr     cpsr_c, r0
    ldr     sp, =__sp_irq
    mov     r0, #0x1F           @ System mode
    msr     cpsr_c, r0
    ldr     sp, =__sp_usr

    @ copy .iwram (code+data) and .data to IWRAM, .ewram to EWRAM
    ldr     r0, =__iwram_lma
    ldr     r1, =__iwram_start
    ldr     r2, =__iwram_end
    bl      copy_words
    ldr     r0, =__ewram_lma
    ldr     r1, =__ewram_start
    ldr     r2, =__ewram_end
    bl      copy_words
    @ zero .bss and .sbss
    ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    bl      zero_words
    ldr     r1, =__sbss_start
    ldr     r2, =__sbss_end
    bl      zero_words

    ldr     r0, =isr_master
    ldr     r1, =0x03007FFC
    str     r0, [r1]

    ldr     r0, =main
    mov     lr, pc
    bx      r0
1:  b       1b

copy_words:
    cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     copy_words
    bx      lr
zero_words:
    mov     r0, #0
2:  cmp     r1, r2
    strlo   r0, [r1], #4
    blo     2b
    bx      lr
    .pool

@ Minimal IRQ handler: acknowledge, tell the BIOS (for VBlankIntrWait), and on
@ VBlank run vblank_isr (video.c).
    .section .iwram, "ax", %progbits
    .arm
    .align 2
    .global isr_master
isr_master:
    mov     r0, #0x04000000
    ldr     r1, [r0, #0x200]        @ IE | (IF << 16)
    and     r1, r1, r1, lsr #16     @ IE & IF
    add     r3, r0, #0x200
    strh    r1, [r3, #2]            @ acknowledge in REG_IF
    ldrh    r2, [r0, #-8]           @ BIOS IF at 0x03007FF8 (mirror 0x03FFFFF8)
    orr     r2, r2, r1
    strh    r2, [r0, #-8]
    tst     r1, #1                  @ VBlank: re-arm the sky gradient DMA
    bxeq    lr
    ldr     r0, =vblank_isr
    bx      r0                      @ tail call; returns to the BIOS through lr
    .pool

    .section .rodata
    .align 2
    .ascii  "SRAM_V113"             @ tells emulators/flashcarts we use SRAM
    .align 2
