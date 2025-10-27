#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum { false, true } bool;
int col_size = 1024;

int pum_similarity_check(uintptr_t target_addr, size_t chunk_size) {
    bool result = false;

    // Clone the all 1s rows into the maj1 row
    // Clone the target row into the rowclone row
    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");

    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");


    // Clone the target row into the maj3 row
    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");

    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");


    // Clone the baseline row into the maj2 row
    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");

    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );

    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");

    // Make the maj operation
    // Send PuM request to start address
    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x14, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");

    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x14, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");


    // Read 512 bytes starting from start
    unsigned char *bytes = (unsigned char *)target_addr;

    int total_bytes = col_size / 8; // column size in byte

    // Go through the row, reading the data and checking the matching result
    for (size_t i = 0; i < total_bytes; i += chunk_size) {
        bool all_zero = true;

        for (size_t j = 0; j < chunk_size && (i + j) < total_bytes; j++) {
            if (bytes[i + j] != 0)
            {
                return i;
            }
        }
    }
    return -1;
}

int main() {
     // Allocate 64 Megabytes of memory for the test
    // 64 * 1024 * 1024 bytes / sizeof(int)
    size_t num_ints = (size_t)4 * 1024 * 1024 * 1024 / sizeof(int);

    // Check if calloc fails before proceeding
    int *arr = calloc(num_ints, sizeof(int));
    if (arr == NULL) {
        perror("Failed to allocate memory");
        return 1;
    }
    printf("Started the program");
    // Start and end addresses (virtual addresses)
    void *start = arr;
    // Calculate the address of the LAST ELEMENT (num_ints - 1 index)
    void *end   = arr + (num_ints - 1);
    /*
    // Send a request that will trigger write data to all subarray IN rows
    uint64_t init_target_addr = 0;
    asm volatile(
        "mov %0, %%rax\n\t" // opcode 10101, mod 00	memory, reg	101, rm 000 [rax]
        ".byte 0x15, 0x28\n\t"
        :: "r"(init_target_addr)
        : "rax", "eax","memory"
    );
    // Reset rax to zero (or any safe value)
    asm volatile("xor %%rax, %%rax" ::: "rax");
    */
    // Initialize the current calculation pointer to start addr of allocation data
    uintptr_t current_addr = (uintptr_t) start; // Use uintptr_t for current_addr
    uintptr_t end_addr = (uintptr_t) end; // Cast end once for the loop condition

    printf("start: %p, end: %p, current_addr: %lx\n", start, end, current_addr);

    while (current_addr <= end_addr)
    {
        // Do the calculations
        // Should return the index offset of the match from current addr
        // If no match was found return -1
        int result = pum_similarity_check(current_addr, sizeof(int));

        // Exit after we found a match
        if (result != -1){
            printf("found a match??? shouldnt happen\n");
            // NOTE: You are returning an address here, not an exit code
            free(arr);
            return current_addr + result;
        }
        else // increase the current addr for the next row
            current_addr += col_size;

    }
    printf("finished\n");
    free(arr); // Clean up allocated memory
    // Exit normally after the entire data set was processed with no match
    return 0;
}
