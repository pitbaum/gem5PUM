#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

/* ===================== Config ===================== */
#define MMIO_BASE       0x200000000ULL       // 8 GiB boundary
#define MMIO_SIZE       (64ULL << 30)        // 64 GiB PUM Address Space

#define CHANNELS        1
#define RANKS_PER_CH    4
#define BANKS_PER_RANK  16

/* User parameters */
const uint64_t amount_of_GiB_allocated = 16;      // how much MMIO to sweep
const int      data_size_bits          = 1024;    // element size (bits)
const int      subarray_row            = 1024;    // rows per subarray
const int      subarray_col            = 1024;    // bits per row

/* Derived geometry */
const int      total_banks        = BANKS_PER_RANK * RANKS_PER_CH;
const uint64_t data_in_alloc      =
    (amount_of_GiB_allocated * (1ULL << 30)) /
    ((uint64_t)data_size_bits * (uint64_t)subarray_col / 8ULL); // How many data instances did we allocate
const uint64_t steps_per_bank     = (data_in_alloc + total_banks - 1) / total_banks; // ceiling division
const uint64_t Bank_stride        = MMIO_SIZE / total_banks;      // How big is a single bank
const uint64_t subarray_stride_bytes =
    (uint64_t)subarray_col * (uint64_t)subarray_row / 8ULL;       // bytes per subarray
const uint64_t step_stride_bytes  =
    (uint64_t)data_size_bits * (uint64_t)subarray_col / 8ULL;

// For compare(): number of 64-bit words per compressed row
#define WORDS_PER_SUBARRAY  ((subarray_col + 63) / 64)

/* ===================== Custom instructions ===================== */

// Helper function to issue the rowclone instruction
static inline void rowclone(uint64_t addr, uint64_t value)
{
    asm volatile (
        ".insn s 0x0b, 0, %1, 0(%0)\n"
        :
        : "r"(addr),
          "r"(value)
        : /* no clobbers */
    );
}

// Helper function to issue the majority instruction
static inline void majority(uint64_t addr, uint64_t value)
{
    asm volatile (
        ".insn s 0x0b, 1, %1, 0(%0)\n"
        :
        : "r"(addr),
          "r"(value)
        : /* no clobbers */
    );
}

/* ===================== Compare ===================== */

/*
 * Compare two compressed rows:
 * return true if there exists at least one bit with a=1, b=0.
 * We directly read from the MMIO addresses instead of memcpy'ing to stack.
 */
static inline bool compare(uint64_t ANDAddr, uint64_t ORAddr)
{
    const volatile uint64_t *a = (const volatile uint64_t *)ANDAddr;
    const volatile uint64_t *b = (const volatile uint64_t *)ORAddr;

    // WORDS_PER_SUBARRAY is a compile-time constant; O3 can unroll this.
#pragma GCC unroll 16
    for (int w = 0; w < WORDS_PER_SUBARRAY; ++w) {
        uint64_t aw = a[w];
        uint64_t bw = b[w];
        uint64_t mask = aw & ~bw;
        if (mask != 0) {
            // there is at least one bit with a=1, b=0
            return true;
        }
    }
    // all bits satisfy !(a==1 && b==0)
    return false;
}

/* ===================== Main PUM loop ===================== */

// Function that will go over all the data allocated and compress it with PUM
// functions and then call compare.
void compress_and_compare(void)
{
    uint64_t current_addr = MMIO_BASE;

    // Phase 1: issue all PUM commands for all steps and banks
    for (uint64_t step = 0; step < steps_per_bank; ++step) {
        uint64_t step_base = current_addr;  // if you intended per-step shift, use step_base += step_stride_bytes;

        for (int i = 0; i < data_size_bits; ++i) {
            const bool is_special = (i == 0) || (i == data_size_bits / 2);

            if (is_special) {
                // First iteration and middle iteration: copy the value into the Out rows
                uint64_t addr = step_base;
                for (int bank = 0; bank < total_banks; ++bank, addr += Bank_stride) {
                    rowclone(addr, addr);
                }
            } else {
                // General case: RC const, RC data, then MAJ on all banks
                uint64_t addr;

                // ROWCLONE const for all banks
                addr = step_base;
                for (int bank = 0; bank < total_banks; ++bank, addr += Bank_stride) {
                    rowclone(addr, addr);
                }

                // ROWCLONE data into Comp row on all banks
                addr = step_base;
                for (int bank = 0; bank < total_banks; ++bank, addr += Bank_stride) {
                    rowclone(addr, addr);
                }

                // Compute MAJORITY on all banks
                addr = step_base;
                for (int bank = 0; bank < total_banks; ++bank, addr += Bank_stride) {
                    majority(addr, addr);
                }
            }
        }
    }

    // Phase 2: read back and compare for each step and bank
    current_addr = MMIO_BASE;
    for (uint64_t step = 0; step < steps_per_bank; ++step) {
        uint64_t addr = current_addr;

        for (int bank = 0; bank < total_banks; ++bank) {
            // AND row at addr, OR row at addr + subarray_col/8
            volatile bool comp = compare(addr, addr + (uint64_t)subarray_col / 8ULL);
            (void)comp; // suppress unused warning
            addr += Bank_stride;
        }

        current_addr += step_stride_bytes;
    }
}

int main(void)
{
    compress_and_compare();
    return 0;
}
