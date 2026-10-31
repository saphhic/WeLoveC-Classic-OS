/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 30th 2026
 * Description: Defines the entry point and section layout for user-space ELF executables.
 */

ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x400000;
    .text : ALIGN(0x1000)
    {
        *(.text.entry)
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :text

    . = ALIGN(0x1000);
    .data :
    {
        *(.data .data.*)
    } :data
    .bss :
    {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }
}
