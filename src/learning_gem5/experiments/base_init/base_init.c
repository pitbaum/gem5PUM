// build: gcc -O2 -std=c11 -pthread mmio.c -o mmio
// Single channel version
#include <stdint.h>
#include <pthread.h>

int main(void) {
    const uint64_t addr = 0x200000000ULL;
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x15, 0x28\n\t"
        :: "r"(addr)
        : "rax", "memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");

    return 0;
}
