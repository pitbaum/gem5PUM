#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum { false, true } bool;
int col_size = 1024;

// Function that checks similarity using custom PuM row operations
int pum_similarity_check(uintptr_t target_addr, size_t chunk_size) {
    bool result = false;

    // --- Step 1: Clone the all-1s rows into maj1 row ---
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    asm volatile("xor %%rax, %%rax" ::: "rax");

    // --- Step 2: Clone the target row into rowclone row ---
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    asm volatile("xor %%rax, %%rax" ::: "rax");

    // --- Step 3: Clone the target row into maj3 row ---
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    asm volatile("xor %%rax, %%rax" ::: "rax");

    // --- Step 4: Clone the baseline row into maj2 row ---
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    asm volatile("xor %%rax, %%rax" ::: "rax");

    // --- Step 5: Perform maj operation ---
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x14, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    asm volatile("xor %%rax, %%rax" ::: "rax");

    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x14, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    asm volatile("xor %%rax, %%rax" ::: "rax");

    // --- Step 6: Read back memory and check ---
    unsigned char *bytes = (unsigned char *)target_addr;
    int total_bytes = col_size / 8; // column size in bytes


    for (size_t i = 0; i < total_bytes; i += chunk_size) {
        bool all_zero = true;

        for (size_t j = 0; j < chunk_size && (i + j) < total_bytes; j++) {
            if (bytes[i + j] != 0) {
                return i;
            }
        }
    }

    return -1;
}

int main() {
    size_t num_ints = (size_t)4 * 1024 * 1024 * 1024 / sizeof(int);
    int *arr = calloc(num_ints, sizeof(int));

    if (!arr) {
        printf("Memory allocation failed!\n");
        return -1;
    }

    // Virtual address boundaries
    void *start = arr;
    void *end   = arr + (num_ints - 1);

    // --- Step 0: Initialize baseline ---
    // Special command that is in the frontend that will generate write to all the baseline addresses to initi the data
    uint64_t init_target_addr = 8589934592;
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x15, 0x28\n\t"
        :: "r"(init_target_addr)
        : "rax", "eax","memory"
    );
    asm volatile("xor %%rax, %%rax" ::: "rax");

    // Start scanning
    uintptr_t current_addr = (uintptr_t) start;

    while (current_addr <= (uintptr_t) end) {

        // Run similarity check
        int result = pum_similarity_check(current_addr, sizeof(int));

        if (result != -1) {
            printf("Match found at addr %p + offset %d\n", (void*)current_addr, result);
            return current_addr + result;
        }

        // Move to the next row
        current_addr += col_size;
    }

    printf("Scan completed: no matches found in dataset.\n");
    return 0;
}
