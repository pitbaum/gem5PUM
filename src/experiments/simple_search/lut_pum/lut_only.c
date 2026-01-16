#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

const uint64_t subarrays_total = MMIO_SIZE / (uint64_t)subarray_stride_bytes;  // amount of subarrays we have in total

#define WORDS_PER_SUBARRAY ((subarray_col + 63) / 64)

/* ---------------------------- LUT ------------------------------------- */
enum { LUT_OPS = 2 };

/* One operation returns two uint64 values */
typedef struct {
  uint64_t a; /* first returned uint64 */
  uint64_t b; /* second returned uint64 */
} pair_t;

/* One LUT record per subarray: 2 operations */
typedef struct {
  pair_t e[LUT_OPS]; /* e[0..1] */
} sub_lut_t;

typedef struct {
  uint64_t e[LUT_OPS]; /* e[0..1] */
} sub_lut_single;


/* LUT for the specific addresses for the Row specific commands */
static sub_lut_t *LUT_MAJ; // LUT to look up MAJ OR & MAJ AND APA
static sub_lut_t *LUT_CONST; // LUT to trigger CONST 1 & CONST 0 RC
static sub_lut_single *LUT_ANDOR; // LUT to look up the AND & OR stored address for RC
static uint64_t  *LUT_DATARC; // LUT to look up the singular DATA RC row address

/* - Force 64KiB granularity: (addr>>16)<<16
 * - Convert to subarray id: (addr64k - MMIO_BASE) / subarray_stride_bytes
 */
static inline uint64_t sub_index(uint64_t addr) {
  uint64_t addr64k = addr & ~UINT64_C(0xFFFF); //Base address without the subarray (i.e. subarrayxcol /8 bits rounded down)
  return (addr64k - MMIO_BASE) >> 16; // Make to multiple of subarray stride bytes
}

/* Populate one cell: (sub, [0|1]) -> (a,b) */
static inline void lut_set_sub(uint64_t sub_id, uint64_t a, uint64_t b) {
  /* Set the sub entries for all the arrays per this index */
  LUT_MAJ[sub_id].e[0] = (pair_t){.a = a, .b = b};
  LUT_MAJ[sub_id].e[1] = (pair_t){.a = a, .b = b};
  LUT_CONST[sub_id].e[0] = (pair_t){.a = a, .b = b};
  LUT_CONST[sub_id].e[1] = (pair_t){.a = a, .b = b};
  LUT_ANDOR[sub_id].e[0] = a;
  LUT_ANDOR[sub_id].e[1] = b;
  LUT_DATARC[sub_id] = a;
}

/* Lookup by address: returns pointer to the whole subarray record.
 * Caller then uses:
 *   sub_lut_t *r = lut_get(addr);
 *   pair_t p = r->e[op_id];
 *   p.a or p.b;
 */
static inline sub_lut_t *lut_MAJ_get(uint64_t addr, uint32_t sub_index) {
  return &LUT_MAJ[sub_index];
}

static inline sub_lut_t *lut_CONST_get(uint64_t addr, uint32_t sub_index) {
  return &LUT_CONST[sub_index];
}

static inline sub_lut_single *lut_ANDOR_get(uint64_t addr, uint32_t sub_index) {
  return &LUT_ANDOR[sub_index];
}

static inline uint64_t lut_DATARC_get(uint64_t addr, uint32_t sub_index) {
  return LUT_DATARC[sub_index];
}

// Populate all with subarray base address
static void lut_populate_all(void) {
  if (!LUT_MAJ)   LUT_MAJ   = (sub_lut_t *)malloc(subarrays_total * sizeof(*LUT_MAJ));
  if (!LUT_CONST) LUT_CONST = (sub_lut_t *)malloc(subarrays_total * sizeof(*LUT_CONST));
  if (!LUT_ANDOR) LUT_ANDOR = (sub_lut_single *)malloc(subarrays_total * sizeof(*LUT_ANDOR));
  if (!LUT_DATARC)LUT_DATARC= (uint64_t *)malloc(subarrays_total * sizeof(*LUT_DATARC));

  if (!LUT_MAJ || !LUT_CONST || !LUT_ANDOR || !LUT_DATARC) {
    fprintf(stderr, "LUT alloc failed\n");
    abort();
  }

  for (uint64_t sub = 0; sub < subarrays_total; sub++) {
    uint64_t sub_base = MMIO_BASE + (sub * (uint64_t)subarray_stride_bytes);
    (void)sub_base;

    uint64_t a = sub_base; /* placeholder */
    uint64_t b = sub_base; /* placeholder */
    lut_set_sub(sub, a, b);
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
  uint64_t bank_addr[banks_used_compress];
  uint32_t sub_id[banks_used_compress];

  // For each piece of data that needs to be processed in a bank
  for (uint64_t step = 0; step < steps_per_bank; step++) {

    // precomopute bank addresses per step
    uint64_t base = current_addr;
    for (int bank = 0; bank < banks_used_compress; bank++) {
        uint64_t addr = base + (uint64_t)bank * Bank_stride;
        bank_addr[bank] = addr;
        sub_id[bank] = sub_index(addr);
    }

    // For each command we need to issue to that data piece
    for (int i = 0; i < to_compress_size_bits; i++) {

      // For all total banks, issue the necessary command

      bool is_bigger = i>256; 
      if (i == 0 || i == 256) {

        // First iteration and middle iteration, just copy the value into the Out Rows
        for (int bank = 0; bank < banks_used_compress; bank++) {
          const uint64_t addr = bank_addr[bank];
          rowclone(addr, lut_ANDOR_get(addr, sub_id[bank])->e[is_bigger]);
        }

      } else {
        // RC + RC + MAJ
        if (i % 32 == 0) {
          // ROWCLONE Const into Comp row on all banks
          for (int bank = 0; bank < banks_used_compress; bank++) {
            // Get the copy constant
            sub_lut_t *r = lut_CONST_get(bank_addr[bank], sub_id[bank]);
            rowclone(r->e[is_bigger].a, r->e[is_bigger].b);
          }
        }

        // ROWCLONE data into Comp row on all banks
        for (int bank = 0; bank < banks_used_compress; bank++) {
          const uint64_t addr = bank_addr[bank];
          rowclone(addr, lut_DATARC_get(addr, sub_id[bank]));
        }

        // Compute MAJORITY on all banks
        for (int bank = 0; bank < banks_used_compress; bank++) {
          // Get the MAJ Execution for this subarray
          sub_lut_t *r = lut_MAJ_get(bank_addr[bank], sub_id[bank]);
          majority(r->e[is_bigger].a, r->e[is_bigger].b);
        }
      }
    }

    // advance the base address
    current_addr += step_stride_bytes;
  }
}

int main(void) {
  // Create the LuT
  lut_populate_all();
  // Go through all data
  compress_and_compare();
  return 0;
}
