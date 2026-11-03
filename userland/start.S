/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 30th 2026
 * Description: Provides the startup entry point for user programs in x86-64 assembly.
 */

.section .text.entry,"ax"
.global _start
.type _start,@function
.extern userland_main
.extern __preinit_array_start
.extern __preinit_array_end
.extern __init_array_start
.extern __init_array_end
.extern __fini_array_start
.extern __fini_array_end

_start:
    xor %rbp, %rbp
    mov (%rsp), %r12
    lea 8(%rsp), %r13

    lea __preinit_array_start(%rip), %rbx
    lea __preinit_array_end(%rip), %r14
.Lrun_preinit_array:
    cmp %r14, %rbx
    jae .Lrun_init_array_start
    mov (%rbx), %rax
    add $8, %rbx
    call *%rax
    jmp .Lrun_preinit_array

.Lrun_init_array_start:
    lea __init_array_start(%rip), %rbx
    lea __init_array_end(%rip), %r14
.Lrun_init_array:
    cmp %r14, %rbx
    jae .Lrun_main
    mov (%rbx), %rax
    add $8, %rbx
    call *%rax
    jmp .Lrun_init_array

.Lrun_main:
    mov %r12, %rdi
    mov %r13, %rsi
    call userland_main

    lea __fini_array_start(%rip), %rbx
    lea __fini_array_end(%rip), %r14
.Lrun_fini_array:
    cmp %r14, %rbx
    jae .Lexit
    sub $8, %r14
    mov (%r14), %rax
    call *%rax
    jmp .Lrun_fini_array

.Lexit:
    xor %edi, %edi
    xor %eax, %eax
    syscall
    ud2

.section .note.GNU-stack,"",@progbits
