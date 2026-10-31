/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 30th 2026
 * Description: Contains x86-64 assembly routines for usermode asm in syscall.
 */

# Written by [@saphhic](https://github.com/saphhic)  
# Date: 26 July 2026

.section .text
.global enter_usermode

enter_usermode:
    mov $0x1b, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    
    pushq $0x1b
    pushq %rsi

    pushfq
    popq %rax
    orq $0x200, %rax
    pushq %rax

    pushq $0x23
    pushq %rdi

    iretq

.section .note.GNU-stack,"",@progbits
