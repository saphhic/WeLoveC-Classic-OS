/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 30th 2026
 * Description: Contains x86-64 assembly routines for syscall entry in syscall.
 */

# Written by [@saphhic](https://github.com/saphhic)  
# Date: 26 July 2026

# Field order of the pushes below MUST match `struct syscall_regs_t`
# in syscall.h. RCX and R11 carry the return RIP/RFLAGS. The dispatcher may
# rewrite them and the saved user RSP to enter or return from a signal handler.

.section .text
.global syscall_entry
.extern syscall_dispatch

syscall_entry:
    swapgs
    movq %rsp, %gs:16
    movq %gs:8, %rsp
    cld

    # Append the interrupted user RSP after the general registers in the C frame.
    pushq %gs:16
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

    # Keep the saved register frame on its original stack while aligning
    # RSP for the SysV ABI call into C++.
    movq %rsp, %r12
    movq %rsp, %rdi
    andq $-16, %rsp
    callq syscall_dispatch
    movq %r12, %rsp

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
    popq %gs:16

    movq %gs:16, %rsp
    swapgs
    sysretq

.section .note.GNU-stack,"",@progbits
