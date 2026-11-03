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
        *(.gcc_except_table .gcc_except_table.*)
        *(.eh_frame_hdr)
        *(.eh_frame .eh_frame.*)
    } :text

    . = ALIGN(0x1000);
    .data :
    {
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)
    } :data
    .preinit_array : ALIGN(8)
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } :data
    .init_array : ALIGN(8)
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :data
    .fini_array : ALIGN(8)
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data
    .bss :
    {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
    }
}
