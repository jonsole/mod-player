/* Boot loader entry points, at BOOT_BASE (firmware/hw.h):
 *   BOOT_BASE + 0  power-on (the CPU's reset address in the FPGA image)
 *   BOOT_BASE + 4  from the firmware (its B command)
 * Both set the stack, clear .bss and call boot_main(from_firmware). */
    .section .text.start
    .global _start
_start:
    j    cold
    j    warm
cold:
    li   s0, 0
    j    1f
warm:
    li   s0, 1
1:  la   sp, _stack_top
    la   t0, _bss_start
    la   t1, _bss_end
2:  bgeu t0, t1, 3f
    sw   zero, 0(t0)
    addi t0, t0, 4
    j    2b
3:  mv   a0, s0
    call boot_main
4:  j    4b
