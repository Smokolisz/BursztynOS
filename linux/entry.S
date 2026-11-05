.section .text
.code64

.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    movq %rsp, syscall_user_rsp(%rip)
    movq syscall_kernel_rsp(%rip), %rsp
    pushq $0x1B
    pushq syscall_user_rsp(%rip)
    pushq %r11
    pushq $0x23
    pushq %rcx
    pushq $0
    pushq $0x80
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rdi
    pushq %rsi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    cld
    call syscall_dispatch
    jmp interrupt_return

.global context_switch
.type context_switch, @function
context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.global kernel_task_trampoline
.type kernel_task_trampoline, @function
kernel_task_trampoline:
    movq %r13, %rdi
    call *%r12
    ud2

.global process_task_trampoline
.type process_task_trampoline, @function
process_task_trampoline:
    movq %r13, %rdi
    call *%r12
    jmp interrupt_return

.section .note.GNU-stack, "", @progbits
