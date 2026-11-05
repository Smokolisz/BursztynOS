/*
 * Mechanizm: Skok z Jądra (Ring 0) do Programu Użytkownika (Ring 3)
 */

.global przejdz_do_ring3

.section .text
.code64

/*
 * extern "C" void przejdz_do_ring3(uint64_t rip, uint64_t rsp);
 * RDI = Punkt wejścia (RIP z loadera)
 * RSI = Wirtualny szczyt stosu Ring 3 (RSP z loadera)
 */
przejdz_do_ring3:
    mov $0x1B, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    pushq $0x1B      
    pushq %rsi       
    
    pushfq
    popq %rax
    orq $0x200, %rax
    pushq %rax       
    
    pushq $0x23      
    pushq %rdi       

    iretq
