/* The boot loader: the top 4 KB of the firmware RAM, from BOOT_BASE
 * (firmware/hw.h; ADR-0011). Its stack is at the top of the same 4 KB, so
 * it never touches the firmware's RAM below. */
ENTRY(_start)
MEMORY { BOOT (rwx) : ORIGIN = 0x0000F000, LENGTH = 4K }

SECTIONS
{
    .text : { KEEP(*(.text.start)) *(.text*) } > BOOT
    .rodata : { *(.rodata*) *(.srodata*) } > BOOT
    .data : { *(.data*) *(.sdata*) } > BOOT
    .bss (NOLOAD) : {
        . = ALIGN(4);
        _bss_start = .;
        *(.bss*) *(.sbss*) *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > BOOT
    _stack_top = ORIGIN(BOOT) + LENGTH(BOOT);
    /* leave the stack at least 512 bytes */
    ASSERT(_bss_end <= _stack_top - 512, "boot loader too large for its 4 KB")
}
