#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================== Config ===================== */
#define MMIO_BASE 0x200000000ULL       // 8 GiB boundary
#define MMIO_SIZE (4ULL << 30)        // 64 GiB PUM Address Space

#define CHANNELS 1
#define RANKS_PER_CH 1
#define BANKS_PER_RANK 4

/* ===================== Derived geometry ===================== */
const int total_banks = BANKS_PER_RANK * RANKS_PER_CH;

/* ===================== User parameters ===================== */
const uint64_t amount_of_GiB_allocated = 32;  // how much MMIO to sweep
const int data_size_bits = 512;               // element size (bits)
const int to_compress_size_bits = 512;        // how many of the element bits to compress
const int subarray_row = 512;                 // rows per subarray
const int subarray_col = 1024;                // columns per row
const int width = 4;                         // bits per column
const int banks_used_compress = total_banks;

// How many rows do we need to operate on to have gone through every single piece of data (round up)
const uint64_t data_in_alloc =
    (amount_of_GiB_allocated * (1ULL << 30)) / ((data_size_bits * subarray_col * width) / 8);

// IF we split all of these rows necessary to activate onto all possible banks we have available 
const uint64_t steps_per_bank =
    (data_in_alloc + total_banks - 1) / total_banks;

const uint64_t Bank_stride = MMIO_SIZE / total_banks;  // How big is a single bank, so we know how to jump between banks for addresses

// how many bytes to shift the address to get the next subarray base address (no width necessary since width is not exposed on the columns)
const int subarray_stride_bytes = (subarray_col * subarray_row) / 8;

#define WORDS_PER_SUBARRAY ((subarray_col + 63) / 64)

/* ===================== Additional LuT keys ===================== */
// MAJ Const ANDout Comp operation identifier -> return APA addresses
const int ANDOutB1Comp = 0;

// MAJ Const ORout Comp operation identifier -> return APA addresses
const int OROutB1Comp = 1;

// Subarrays ANDOut Address -> return Address of this subarrays ANDout row
const int ANDOutAddr = 2;

// Subarrays OROut Address -> return Address of this subarrays ORout row
const int OROutAddr = 3;

// Subarrays Const Address -> return Address of this subarrays Const row
const int ConstAddr = 4;

// Subarrays Comp Address -> return Address of this subarrays Comp row
const int CompAddr = 5;

/* ===================== PUM instruction helpers ===================== */

// Helper function to issue the rowclone instruction
static inline void rowclone(uint64_t addr, uint64_t value) {
  // LUT lookup to get the physical address given the subarray id and the Rowfunction id
  asm volatile(".insn s 0x0b, 0, %1, 0(%0)\n" : : "r"(addr), "r"(value) :);
}

// Helper function to issue the majority instruction
static inline void majority(uint64_t addr, uint64_t value) {
  asm volatile(".insn s 0x0b, 1, %1, 0(%0)\n" : : "r"(addr), "r"(value) :);
}

/* ===================== Main kernel ===================== */

// Function that will go over all the data allocated and compress it with PUM functions
// and then call the compare
void compress_and_compare(void) {
  // Issue all for a single bank and then issue per symmetry to all banks/ranks/channels
  // over the flat address space.
  //
  // After each step we should advance (subarray_col * data_size_bits) bits in the address.

  uint64_t current_addr = MMIO_BASE;

  // For each piece of data that needs to be processed in a bank
  for (uint64_t step = 0; step < steps_per_bank; step++) {

    // For each command we need to issue to that data piece
    for (int i = 0; i < to_compress_size_bits; i++) {

      // For all total banks, issue the necessary command
      uint64_t addr = current_addr;

      if (i == 0 || i == to_compress_size_bits / 2) {
        // First iteration and middle iteration: just copy the value into the Out Rows

        addr = current_addr;
        for (int bank = 0; bank < banks_used_compress; bank++) {
          rowclone(addr, ANDOutAddr);
          addr += Bank_stride;
        }

        addr = current_addr;  // reset addr
      } else {
        // RC + RC + MAJ

        if (i > 0 && i != to_compress_size_bits / 2 && (i % 32) == 0) {
          addr = current_addr;  // reset addr

          // ROWCLONE Const into Comp row on all banks
          for (int bank = 0; bank < banks_used_compress; bank++) {
            rowclone(addr, ConstAddr);
            addr += Bank_stride;  // go to next bank
          }
        }

        addr = current_addr;  // reset addr

        // ROWCLONE data into Comp row on all banks
        for (int bank = 0; bank < banks_used_compress; bank++) {
          rowclone(addr, CompAddr);
          addr += Bank_stride;  // go to next bank
        }

        addr = current_addr;  // reset addr

        // Compute MAJORITY on all banks
        for (int bank = 0; bank < banks_used_compress; bank++) {
          majority(addr, ANDOutAddr);
          addr += Bank_stride;  // go to next bank
        }
      }
    }

    // advance the base address
    current_addr += subarray_stride_bytes;
  }
}

int main(void) {
  // Create the LuT
  // Go through all data
  compress_and_compare();
  return 0;
}
