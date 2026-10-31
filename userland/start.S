/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 30th 2026
 * Description: Provides the startup entry point for user programs in x86-64 assembly.
 */

.section .text.entry,"ax"
.global _start
.type _start,@function
.extern userland_main

_start:
    xor %rbp, %rbp
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    call userland_main
    xor %edi, %edi
    xor %eax, %eax
    syscall
    ud2

.section .note.GNU-stack,"",@progbits
