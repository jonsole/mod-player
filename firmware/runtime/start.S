/* Entry: set the stack, clear .bss, run main. The boot loader (or, in the
 * simulations, the block RAM's initial contents) puts the whole image (code
 * and initialised data) at address 0, so there is nothing to copy. */
    .section .text.start
    .global _start
_start:
    la   sp, _stack_top
    la   t0, _bss_start
    la   t1, _bss_end
1:  bgeu t0, t1, 2f
    sw   zero, 0(t0)
    addi t0, t0, 4
    j    1b
2:  call main
3:  j    3b
