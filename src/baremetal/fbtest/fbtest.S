@ fbtest - first code to run on the FOSTON FS-707DC without Windows CE.
@ Entered by the bfagent loader at PA 0x00008000 with MMU and caches off.
@ Paints moving colour bands into the framebuffer Windows CE left configured
@ (PA 0x03E00030, 800x480 RGB565, measured in docs/01-hardware.md) forever.
@ Seeing the bands move proves the loader handed the CPU over correctly.
@ SPDX-License-Identifier: GPL-3.0-or-later
        .text
        .global _start
_start:
        ldr     r8, =0x03E00030         @ framebuffer
        mov     r9, #0                  @ frame counter
frame:
        mov     r0, r8
        mov     r1, #0                  @ y
row:
        add     r2, r1, r9
        mov     r2, r2, lsr #5          @ band = (y + frame) / 32
        and     r2, r2, #7
        adr     r3, colours
        ldr     r2, [r3, r2, lsl #2]    @ two pixels of the band colour
        mov     r4, #400                @ 800 pixels = 400 words
col:
        str     r2, [r0], #4
        subs    r4, r4, #1
        bne     col
        add     r1, r1, #1
        cmp     r1, #480
        blt     row
        add     r9, r9, #4
        b       frame
        .ltorg
colours:
        .word   0x343C343C              @ BigLinux blue
        .word   0x07FF07FF              @ cyan
        .word   0x21042104              @ dark grey
        .word   0xFFFFFFFF              @ white
        .word   0x18E318E3              @ graphite
        .word   0x07E007E0              @ green
        .word   0xFFE0FFE0              @ yellow
        .word   0xF800F800              @ red
