/*
 * Relocation-free instruction streams for comparing BL616 Flash XIP with
 * cached OCRAM.  Each function is copied byte-for-byte to OCRAM at runtime.
 * Keep every instruction 32 bits so the declared working-set sizes are exact.
 */

    .section .text.m61_membench_4k, "ax", @progbits
    .balign 32
    .option push
    .option norvc
    .option norelax
    .global m61_membench_4k_start
    .global m61_membench_4k_end
    .type m61_membench_4k_start, @function
m61_membench_4k_start:
    lui     t0, 0x13579
    addi    t0, t0, 0x246
    .rept 255
    add     a0, a0, t0
    xor     t0, t0, a0
    addi    a0, a0, 1
    xor     a0, a0, t0
    .endr
    xor     a0, a0, t0
    jalr    zero, 0(ra)
m61_membench_4k_end:
    .size m61_membench_4k_start, . - m61_membench_4k_start
    .if (m61_membench_4k_end - m61_membench_4k_start) != 4096
    .error "m61 4 KiB instruction benchmark has the wrong size"
    .endif
    .option pop

    .section .text.m61_membench_40k, "ax", @progbits
    .balign 32
    .option push
    .option norvc
    .option norelax
    .global m61_membench_40k_start
    .global m61_membench_40k_end
    .type m61_membench_40k_start, @function
m61_membench_40k_start:
    lui     t0, 0x13579
    addi    t0, t0, 0x246
    .rept 2559
    add     a0, a0, t0
    xor     t0, t0, a0
    addi    a0, a0, 1
    xor     a0, a0, t0
    .endr
    xor     a0, a0, t0
    jalr    zero, 0(ra)
m61_membench_40k_end:
    .size m61_membench_40k_start, . - m61_membench_40k_start
    .if (m61_membench_40k_end - m61_membench_40k_start) != 40960
    .error "m61 40 KiB instruction benchmark has the wrong size"
    .endif
    .option pop

    .section .note.GNU-stack, "", @progbits
