/* Written by [@saphhic](https://github.com/saphhic)
 * Date: October 30th 2026
 * Description: Contains x86-64 assembly routines for enter kernel in WeLoveC Classic.
 */

.text
.p2align 12
.globl enter_kernel
.def enter_kernel; .scl 2; .type 32; .endef
enter_kernel:
    cli
    mov %rcx, %r10
    mov %rdx, %r11
    mov %r8, %r12

    /* Replace the firmware GDT before firmware mappings are discarded. */
    sub $16, %rsp
    movw $(gdt_end - gdt - 1), (%rsp)
    lea gdt(%rip), %rax
    mov %rax, 2(%rsp)
    lgdt (%rsp)
    add $16, %rsp
    pushq $0x08
    lea 1f(%rip), %rax
    push %rax
    lretq
1:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    xor %eax, %eax
    xor %edx, %edx
    mov $0xc0000100, %ecx
    wrmsr
    inc %ecx
    wrmsr
    inc %ecx
    wrmsr

    /* Every exception and NMI gets a safe, non-returning halt handler. */
    lea fatal_halt(%rip), %rbx
    lea idt_table(%rip), %rsi
    mov $256, %ecx
2:
    movw %bx, 0(%rsi)
    movw $0x08, 2(%rsi)
    movb $0, 4(%rsi)
    movb $0x8e, 5(%rsi)
    mov %rbx, %rdx
    shr $16, %rdx
    movw %dx, 6(%rsi)
    mov %rbx, %rdx
    shr $32, %rdx
    movl %edx, 8(%rsi)
    movl $0, 12(%rsi)
    add $16, %rsi
    loop 2b
    sub $16, %rsp
    movw $4095, (%rsp)
    lea idt_table(%rip), %rax
    mov %rax, 2(%rsp)
    lidt (%rsp)
    add $16, %rsp

    test $1, %r9b
    jz 3f
    mov $0xc0000080, %ecx
    rdmsr
    or $0x800, %eax
    wrmsr
3:
    /* Use PAT entry 1 (selected by PWT=1, PCD=0) for the GOP framebuffer.
       Preserve the other PAT entries and fall back to UC when PAT is absent. */
    test $2, %r9b
    jz 4f
    wbinvd
    mov $0x277, %ecx
    rdmsr
    and $0xffff00ff, %eax
    or $0x100, %eax
    wrmsr
    wbinvd
4:
    /* Flush global translations without changing the firmware LA57 mode. */
    mov %cr4, %rax
    and $-129, %rax
    mov %rax, %cr4
    mov %r10, %cr3
    mov %cr0, %rax
    or $0x10000, %rax
    mov %rax, %cr0
    mov %r12, %rcx
    xor %rbp, %rbp
    jmp *%r11

.globl fatal_halt
.def fatal_halt; .scl 2; .type 32; .endef
fatal_halt:
    cli
4:
    hlt
    jmp 4b

.section .rdata,"dr"
.p2align 3
gdt:
    .quad 0
    .quad 0x00af9a000000ffff
    .quad 0x00cf92000000ffff
gdt_end:

.section .bss
.p2align 12
idt_table:
    .skip 4096
