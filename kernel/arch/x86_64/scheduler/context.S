/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 30th 2026
 * Description: Contains x86-64 assembly routines for context in scheduler.
 */

.section .text
.extern scheduler_current_fx_state

.global scheduler_reschedule_stub
scheduler_reschedule_stub:
    pushq $0
    pushq $0xF0

    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax

    testb $3, 144(%rsp)
    jz 1f
    swapgs
1:
    movq scheduler_current_fx_state(%rip), %rdi
    fxsave64 (%rdi)
    movq %rsp, %rdi
    andq $-16, %rsp
    callq scheduler_reschedule_dispatch
    movq %rax, %rsp
    testb $3, 144(%rsp)
    jz 2f
    swapgs
2:
    movq scheduler_current_fx_state(%rip), %rdi
    fxrstor64 (%rdi)

    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15

    addq $16, %rsp
    iretq

.global scheduler_task_trampoline
scheduler_task_trampoline:
    callq scheduler_task_bootstrap
    ud2

.section .note.GNU-stack,"",@progbits
