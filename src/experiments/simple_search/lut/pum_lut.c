#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================== Config ===================== */
#define MMIO_BASE 0x200000000ULL  // 8 GiB boundary
#define MMIO_SIZE (64ULL << 30)   // 64 GiB PUM Address Space

#define CHANNELS 1
#define RANKS_PER_CH 4
#define BANKS_PER_RANK 16

/* ===================== User parameters ===================== */
const uint64_t amount_of_GiB_allocated = 32;  // how much MMIO to sweep
const int data_size_bits = 512;               // element size (bits)
const int to_compress_size_bits = 512;        // How many of the element size do we actually compress
const int subarray_row = 512;                 // rows per subarray
const int subarray_col = 1024;                // bits per row
const int banks_used_compress = 64;

/* ===================== Derived geometry ===================== */
const int total_banks = BANKS_PER_RANK * RANKS_PER_CH;

const uint64_t data_in_alloc =
    (amount_of_GiB_allocated * (1ULL << 30)) / ((data_size_bits * subarray_col) / 8);
// How many data instances did we allocate

const uint64_t steps_per_bank =
    (data_in_alloc + total_banks - 1) / total_banks;
// Fast ceiling division, steps we take per bank (all the non symmetric operations)

const uint64_t Bank_stride = MMIO_SIZE / total_banks;  // How big is a single bank

const int subarray_stride_bytes = (subarray_col * subarray_row) / 8;
// how many bytes to shift the address to get the next subarray base address

const int step_stride_bytes = (data_size_bits * subarray_col) / 8;

const uint64_t subarrays_total =
    MMIO_SIZE / (uint64_t)subarray_stride_bytes;  // amount of subarrays we have in total

#define WORDS_PER_SUBARRAY ((subarray_col + 63) / 64)

/* ===================== Additional LuT keys ===================== */
// MAJ Const ANDout Comp operation identifier -> return APA addresses
const int ANDOutB1Comp = 0;

// MAJ Const ORout Comp operation identifier -> return APA addresses
const int OROutB1Comp = 1;

// Suabarrays Const Address -> return Address of this subarrays Const row
const int CopyConstant0 = 2;

// Subarrays Comp Address -> return Address of this subarrays Comp row
const int CopyConstant1 = 3;

// Subarrays ANDOut Address -> return Address of this subarrays ANDout row
const int ANDOutAddr = 5;

// Subarrays OROut Address -> return Address of this subarrays ORout row
const int OROutAddr = 3;

/* ---------------------------- LUT ------------------------------------- */
/* We have 4 operations per subarray (op_id 0..3).
 * We will return the whole subarray record so the caller can pick e[op_id].
 */
enum { LUT_OPS = 4 };

/* One operation returns two uint64 values */
typedef struct {
  uint64_t a; /* first returned uint64 */
  uint64_t b; /* second returned uint64 */
} pair_t;

/* One LUT record per subarray: 4 operations */
typedef struct {
  pair_t e[LUT_OPS]; /* e[0..3] */
} sub_lut_t;

/* LUT[sub] is the record for subarray "sub" */
static sub_lut_t *LUT = NULL;
static uint64_t LUT_SUBS = 0;

/* - Force 64KiB granularity: (addr>>16)<<16
 * - Convert to subarray id: (addr64k - MMIO_BASE) / subarray_stride_bytes
 */
static inline uint64_t sub_index(uint64_t addr) {
  uint64_t addr64k = (addr >> 16) << 16;
  return (addr64k - MMIO_BASE) / (uint64_t)subarray_stride_bytes;
}

/* Populate one cell: (sub, op_id) -> (a,b) */
static inline void lut_set_sub(uint64_t sub, uint8_t op_id, uint64_t a, uint64_t b) {
  /* Keep fast; caller ensures bounds. */
  LUT[sub].e[op_id] = (pair_t){.a = a, .b = b};
}

/* Lookup by address: returns pointer to the whole subarray record.
 * Caller then uses:
 *   sub_lut_t *r = lut_get(addr);
 *   pair_t p = r->e[op_id];
 */
static inline sub_lut_t *lut_get(uint64_t addr) {
  if (!LUT) return NULL;
  uint64_t sub = sub_index(addr);
  if (sub >= LUT_SUBS) return NULL;
  return &LUT[sub];
}

// Populate all with subarray base address
static void lut_populate_all(void) {
  for (uint64_t sub = 0; sub < LUT_SUBS; sub++) {
    uint64_t sub_base = MMIO_BASE + sub * (uint64_t)subarray_stride_bytes;
    (void)sub_base;

    for (uint8_t op = 0; op < LUT_OPS; op++) {
      uint64_t a = sub; /* placeholder */
      uint64_t b = sub; /* placeholder */
      lut_set_sub(sub, op, a, b);
    }
  }
}

/* ----------------------------- HELPERS --------------------------------------- */

// Helper function to issue the rowclone instruction
static inline void rowclone(uint64_t addr, uint64_t value) {
  asm volatile(".insn s 0x0b, 0, %1, 0(%0)\n" : : "r"(addr), "r"(value) :);
}

// Helper function to issue the majority instruction
static inline void majority(uint64_t addr, uint64_t value) {
  asm volatile(".insn s 0x0b, 1, %1, 0(%0)\n" : : "r"(addr), "r"(value) :);
}

/*------------------------------------ MAIN LOOP ----------------------------------------------- */

// Function that will go over all the data allocated and compress it with PUM functions and than call the compare
void compress_and_compare() {
  // Issue all for a single bank and than issue per symmetry to all banks rank channels over the flat Address space
  // After each step we should advance (subarray_col * data_size_bits) bits in the address

  uint64_t current_addr = MMIO_BASE;

  // For each piece of data that needs to be processed in a bank
  for (int step = 0; step < steps_per_bank; step++) {

    // For each command we need to issue to that data piece
    for (int i = 0; i < to_compress_size_bits; i++) {

      // For all total banks, issue the necessary command
      uint64_t addr = current_addr;

      if (i == 0 || i == to_compress_size_bits / 2) {
        // First iteration and middle iteration, just copy the value into the Out Rows
        addr = current_addr;

        for (int bank = 0; bank < banks_used_compress; bank++) {
          rowclone(addr, ANDOutAddr);
          addr += Bank_stride;
        }

        addr = current_addr;  // reset addr
      } else {
        // RC + RC + MAJ
        if (i > 0 && i != to_compress_size_bits / 2 && i % 32 == 0) {
          addr = current_addr;  // reset addr

          // ROWCLONE Const into Comp row on all banks
          for (int bank = 0; bank < banks_used_compress; bank++) {
            // Get the copy constant
            sub_lut_t *r = lut_get(addr);
            rowclone(r->e[CopyConstant0].a, r->e[CopyConstant0].b);
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
          // Get the MAJ Execution for this subarray
          sub_lut_t *r = lut_get(addr);
          majority(r->e[ANDOutB1Comp].a, r->e[ANDOutB1Comp].b);
          addr += Bank_stride;  // go to next bank
        }
      }
    }

    // advance the base address
    current_addr += step_stride_bytes;
  }
}

int main(void) {
  // Create the LuT
  // Go through all data
  compress_and_compare();
  return 0;
}
