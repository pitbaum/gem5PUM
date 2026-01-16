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

/* ===================== Derived geometry ===================== */
const int total_banks = BANKS_PER_RANK * RANKS_PER_CH; /* (per channel; CHANNELS is 1 here) */

/* ===================== User parameters ===================== */
const uint64_t amount_of_GiB_allocated = 32;  // how much MMIO to sweep
const int data_size_bits = 512;               // element size (bits)
const int to_compress_size_bits = 512;        // how many of the element bits to compress
const int subarray_row = 512;                 // rows per subarray
const int subarray_col = 1024;                // columns per row
const int width = 8;                          // bits per column
const int banks_used_compress = total_banks;

// How many rows do we need to operate on to have gone through every single piece of data (round up)
const uint64_t data_in_alloc =
    (amount_of_GiB_allocated * (1ULL << 30)) / ((data_size_bits * subarray_col * width) / 8);

// IF we split all of these rows necessary to activate onto all possible banks we have available
const uint64_t steps_per_bank =
    (data_in_alloc + total_banks - 1) / total_banks;

const uint64_t Bank_stride = MMIO_SIZE / total_banks;

// how many bytes to shift the address to get the next subarray base address
const int subarray_stride_bytes = (subarray_col * subarray_row) / 8;

const uint64_t subarrays_total = MMIO_SIZE / (uint64_t)subarray_stride_bytes;  // (not used by new LUT sizing)

#define WORDS_PER_SUBARRAY ((subarray_col + 63) / 64)
static volatile uint64_t sink;

/* ===================== Additional LuT keys ===================== */
const int ANDOutB1Comp = 0;
const int OROutB1Comp = 1;
const int CopyConstant0 = 2;
const int CopyConstant1 = 3;
const int ANDOutAddr = 4;
const int OROutAddr = 5;
const int DATARCAddr = 6;

/* ---------------------------- NEW LUT (direct storage) ----------------------------

Physical address order: Ch Ra Ba Row Co
You split Row into:
  - subarray_id (8 bits)  : Row[16:9]
  - row_in_sub (9 bits)   : Row[8:0]
You don't care about Co => treat Co = 0.

We store 11 "special" row offsets per (bank, subarray_id). Each value is stored as a
16-bit *byte offset within the 64KiB subarray*, i.e. (row_in_sub << ROW_SHIFT).
This keeps hot-loop reconstruction as:
    addr = sub_base + offset16;
*/

#define SUBARRAY_ID_BITS 8
#define SUBARRAYS_PER_BANK (1u << SUBARRAY_ID_BITS)  // 256
#define ROW_BYTES ((uint32_t)(subarray_col / 8))      // 1024 bits / 8 = 128 bytes
#define ROW_SHIFT 7u                                 // log2(128) = 7

enum { LUT_LANES = 2 };
enum { LUT_SPECIALS = 11 };

/* Indices into the 11 values (lane 0/1 + datarc). */
enum {
  IDX_MAJ_A0 = 0,
  IDX_MAJ_B0 = 1,
  IDX_CNST_A0 = 2,
  IDX_CNST_B0 = 3,
  IDX_ANDOR0 = 4,

  IDX_MAJ_A1 = 5,
  IDX_MAJ_B1 = 6,
  IDX_CNST_A1 = 7,
  IDX_CNST_B1 = 8,
  IDX_ANDOR1 = 9,

  IDX_DATARC = 10
};

typedef struct __attribute__((aligned(32))) {
  uint16_t off[LUT_SPECIALS];  // byte offsets within subarray (0..65535), typically multiples of 128
} lut_entry_t;

static lut_entry_t *LUT;  // [banks_used_compress][256]

static inline lut_entry_t *lut_get(int bank, uint32_t sub8) {
  return &LUT[(size_t)bank * (size_t)SUBARRAYS_PER_BANK + (size_t)sub8];
}

/* Helper: convert row_in_sub (0..511) to byte offset within subarray. */
static inline uint16_t row_to_off16(uint16_t row_in_sub) {
  return (uint16_t)((uint32_t)row_in_sub << ROW_SHIFT); // fits: 511<<7 = 65408
}

/* Populate one entry for a given (bank, subarray_id). Placeholder values for now. */
static inline void lut_set_key(int bank, uint32_t sub8,
                               uint16_t maj_a0_row, uint16_t maj_b0_row,
                               uint16_t maj_a1_row, uint16_t maj_b1_row,
                               uint16_t cnst_a0_row, uint16_t cnst_b0_row,
                               uint16_t cnst_a1_row, uint16_t cnst_b1_row,
                               uint16_t andor0_row, uint16_t andor1_row,
                               uint16_t datarc_row) {
  lut_entry_t *e = lut_get(bank, sub8);

  e->off[IDX_MAJ_A0]  = row_to_off16(maj_a0_row);
  e->off[IDX_MAJ_B0]  = row_to_off16(maj_b0_row);
  e->off[IDX_CNST_A0] = row_to_off16(cnst_a0_row);
  e->off[IDX_CNST_B0] = row_to_off16(cnst_b0_row);
  e->off[IDX_ANDOR0]  = row_to_off16(andor0_row);

  e->off[IDX_MAJ_A1]  = row_to_off16(maj_a1_row);
  e->off[IDX_MAJ_B1]  = row_to_off16(maj_b1_row);
  e->off[IDX_CNST_A1] = row_to_off16(cnst_a1_row);
  e->off[IDX_CNST_B1] = row_to_off16(cnst_b1_row);
  e->off[IDX_ANDOR1]  = row_to_off16(andor1_row);

  e->off[IDX_DATARC]  = row_to_off16(datarc_row);
}

/* New populate: allocate only banks_used_compress * 256 entries, not subarrays_total. */
static void lut_populate_all(void) {
  if (!LUT) {
    LUT = (lut_entry_t *)malloc((size_t)banks_used_compress * (size_t)SUBARRAYS_PER_BANK * sizeof(*LUT));
  }
  if (!LUT) {
    fprintf(stderr, "LUT alloc failed\n");
    abort();
  }

  /* Placeholder: all specials point to row 0 (offset 0). Replace these with your real special-row map. */
  for (int bank = 0; bank < banks_used_compress; bank++) {
    for (uint32_t sub8 = 0; sub8 < SUBARRAYS_PER_BANK; sub8++) {
      lut_set_key(bank, sub8,
                  /* maj lane0 */ 0, 0,
                  /* maj lane1 */ 0, 0,
                  /* cnst lane0 */ 0, 0,
                  /* cnst lane1 */ 0, 0,
                  /* andor */ 0, 0,
                  /* datarc */ 0);
    }
  }
}

/* ----------------------------- HELPERS --------------------------------------- */

static inline void rowclone(uint64_t addr, uint64_t value) {
  asm volatile(".insn s 0x0b, 0, %1, 0(%0)\n" : : "r"(addr), "r"(value) :);
}
static inline void majority(uint64_t addr, uint64_t value) {
  asm volatile(".insn s 0x0b, 1, %1, 0(%0)\n" : : "r"(addr), "r"(value) :);
}

/*------------------------------------ MAIN LOOP ----------------------------------------------- */

void compress_and_compare() {
  uint64_t current_addr = MMIO_BASE;
  uint64_t acc = 0;

  uint64_t sub_base[banks_used_compress];

  for (uint64_t step = 0; step < steps_per_bank; step++) {

    /* In this code, current_addr advances by exactly one subarray (64KiB) per step.
       With your "Row split": subarray_id is the high 8 bits of Row, which corresponds to step modulo 256. */
    uint32_t sub8 = (uint32_t)(step & (SUBARRAYS_PER_BANK - 1u));

    /* precompute sub_base per bank for this step */
    uint64_t base = current_addr;
    for (int bank = 0; bank < banks_used_compress; bank++) {
      uint64_t addr = base + (uint64_t)bank * Bank_stride;
      sub_base[bank] = addr & ~UINT64_C(0xFFFF);  // 64KiB-aligned subarray base
    }

    for (int i = 0; i < to_compress_size_bits; i++) {

      bool is_bigger = (i > 256);
      int lane = is_bigger ? 1 : 0;

      if (i == 0 || i == 256) {
        for (int bank = 0; bank < banks_used_compress; bank++) {
          const lut_entry_t *e = lut_get(bank, sub8);
          uint16_t off = e->off[lane ? IDX_ANDOR1 : IDX_ANDOR0];
          acc ^= (sub_base[bank] + (uint64_t)off);
        }

      } else {

        if ((i & 31) == 0) {
          for (int bank = 0; bank < banks_used_compress; bank++) {
            const lut_entry_t *e = lut_get(bank, sub8);
            uint16_t off_a = e->off[lane ? IDX_CNST_A1 : IDX_CNST_A0];
            uint16_t off_b = e->off[lane ? IDX_CNST_B1 : IDX_CNST_B0];
            acc ^= (sub_base[bank] + (uint64_t)off_a);
            acc ^= (sub_base[bank] + (uint64_t)off_b);
          }
        }

        for (int bank = 0; bank < banks_used_compress; bank++) {
          const lut_entry_t *e = lut_get(bank, sub8);
          acc ^= (sub_base[bank] + (uint64_t)e->off[IDX_DATARC]);
        }

        for (int bank = 0; bank < banks_used_compress; bank++) {
          const lut_entry_t *e = lut_get(bank, sub8);
          uint16_t off_a = e->off[lane ? IDX_MAJ_A1 : IDX_MAJ_A0];
          uint16_t off_b = e->off[lane ? IDX_MAJ_B1 : IDX_MAJ_B0];
          acc ^= (sub_base[bank] + (uint64_t)off_a);
          acc ^= (sub_base[bank] + (uint64_t)off_b);
        }
      }
    }

    current_addr += subarray_stride_bytes;
  }

  sink = acc;
}

int main(void) {
  lut_populate_all();
  compress_and_compare();
  return 0;
}
