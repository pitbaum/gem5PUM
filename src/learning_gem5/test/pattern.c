#include <stdio.h>
#include <stdint.h>

int main() {
    // random other code
    int a = 1+1;
    printf("Address of a: %p\n", (void*)&a);
    printf("Value of A: %d \n", a);

    // Send PuM request to addr 0xff
    uint64_t addr = 0x0;
    uint32_t eax_val;

    uint64_t rsp_before, rsp_after;

    asm volatile("mov %%rsp, %0" : "=r"(rsp_before));


    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(addr)
        : "rax", "eax","memory"
    );


    asm volatile("mov %%rsp, %0" : "=r"(rsp_after));

    printf("Stack pointer before: %p\n", (void*)rsp_before);
    printf("Stack pointer after:  %p\n", (void*)rsp_after);

    a ++;
    printf("Value of A: %d \n", a);
    printf("Value of A: %d \n", a);
    printf("Address of a: %p\n", (void*)&a);

    // Return success status
    return 0;
}
