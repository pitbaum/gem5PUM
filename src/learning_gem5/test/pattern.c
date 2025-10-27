#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

int main() {
    uint64_t target = 100;
    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(target)
        : "rax", "eax","memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");

        asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(target)
        : "rax", "eax","memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");

        asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(target)
        : "rax", "eax","memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");



    int a = 1;

    int b = 2;
    int c = a + b;

    /*
    size_t num_ints = (size_t)4 * 1024 * 1024 * 1024 / sizeof(int);
    int *arr = calloc(num_ints, sizeof(int));

    if (arr == NULL) {
        printf("Memory allocation failed!\n");
        return 1;
    }

    printf("Successfully allocated memory for %zu integers (~4GB).\n", num_ints);


    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { perror("fopen");
        return 1;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        printf("%s", line);
    }
    fclose(f);


    // Start and end addresses (virtual addresses)
    void *start = arr;
    void *end   = arr + (num_ints - 1);

    printf("Allocated ~4GB for %zu integers\n", num_ints);
    printf("Start address: %p\n", start);
    printf("End address  : %p\n", end);



    // Send PuM request to start address
    long addr = 0x7ffef7dcd010;
    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x14, 0x28\n\t"
        :: "r"(addr)
        : "rax", "eax","memory"
    );
    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");


    int a = 1+1;

    printf("Address of a: %p\n", (void*)&a);
    printf("Value of A: %d \n", a);

    uint32_t eax_val;

    uint64_t rsp_before, rsp_after;

    // Reset rax to zero (or any safe value)
    asm volatile("mov %%rsp, %0" : "=r"(rsp_before));

    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(addr)
        : "rax", "eax","memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");

    asm volatile("mov %%rsp, %0" : "=r"(rsp_after));

    printf("Stack pointer before: %p\n", (void*)rsp_before);
    printf("Stack pointer after:  %p\n", (void*)rsp_after);

    a ++;
    printf("Value of A: %d \n", a);
    printf("Value of A: %d \n", a);
    printf("Address of a: %p\n", (void*)&a);

    // Return success status
    free(arr);
    */
    return 0;
}
