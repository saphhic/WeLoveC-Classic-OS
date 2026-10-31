/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 30th 2026
 * Description: Defines the entry point and section layout for the minimal kernel executable.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

KERNEL_VMA = 0xffffffff80000000;
KERNEL_LMA = 0x00100000;

PHDRS
{
    text   PT_LOAD FLAGS(5); /* read + execute */
    rodata PT_LOAD FLAGS(4); /* read only */
    data   PT_LOAD FLAGS(6); /* read + write */
}

SECTIONS
{
    . = KERNEL_VMA;
    .text : AT(KERNEL_LMA + (ADDR(.text) - KERNEL_VMA))
    {
        KEEP(*(.text.entry))
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);
    .rodata : AT(KERNEL_LMA + (ADDR(.rodata) - KERNEL_VMA))
    {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(0x1000);
    .data : AT(KERNEL_LMA + (ADDR(.data) - KERNEL_VMA))
    {
        *(.data .data.*)
    } :data

    .bss : AT(KERNEL_LMA + (ADDR(.bss) - KERNEL_VMA))
    {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
    }
}
