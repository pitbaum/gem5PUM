// build: gcc -O2 -std=c11 -pthread mmio.c -o mmio
// Several channels version with multithreading
#include <stdint.h>
#include <pthread.h>

static inline void send_mmio(uint64_t addr) {
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x15, 0x28\n\t"
        :: "r"(addr)
        : "rax", "memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");
}

static void* worker(void *arg) {
    send_mmio((uint64_t)(uintptr_t)arg);
    return NULL;
}

int main(void) {
    const uint64_t a0 = 0x200000000ULL;
    const uint64_t a1 = 0x200000040ULL;

    pthread_t th;
    pthread_create(&th, NULL, worker, (void*)(uintptr_t)a1);
    send_mmio(a0);
    pthread_join(th, NULL);

    return 0;
}
