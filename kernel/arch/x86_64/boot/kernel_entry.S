/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 30th 2026
 * Description: Contains x86-64 assembly routines for kernel entry in boot.
 */

.section .text.entry,"ax"
.global kernel_entry
.type kernel_entry,@function
kernel_entry:
    cli
    cld
    mov %rcx, %r12
    lea kernel_stack_top(%rip), %rsp
    and $-16, %rsp
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx
    xor %eax, %eax
    shr $3, %rcx
    rep stosq
    xor %rbp, %rbp
    mov %r12, %rdi
    call kernel_main
1:  hlt
    jmp 1b
.section .bss
.align 16
kernel_stack:
    .skip 65536
.global kernel_stack_top
kernel_stack_top:
