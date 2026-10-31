/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 30th 2026
 * Description: Contains x86-64 assembly routines for tables asm in boot.
 */

# Written by [@saphhic](https://github.com/saphhic)  
# Date: 26 July 2026

#   You cannot reload CS from C/C++ (no instruction loads CS directly);
#   the standard trick in long mode is a far return to a label right
#  after itself, using the new code selector.

.section .text
.global gdt_flush
.global tss_flush
.global idt_flush

gdt_flush:
    lgdt (%rdi)

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    pushq $0x08
    lea 1f(%rip), %rax
    pushq %rax
    lretq
    
1:
    ret

tss_flush:
    mov $0x28, %ax
    ltr %ax
    ret

idt_flush:
    lidt (%rdi)
    ret

.section .note.GNU-stack,"",@progbits



