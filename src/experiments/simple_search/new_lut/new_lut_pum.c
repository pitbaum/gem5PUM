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
const int total_banks = BANKS_PER_RANK * RANKS_PER_CH;

/* ===================== User parameters ===================== */
const uint64_t amount_of_GiB_allocated = 32;  // how much MMIO to sweep
const int data_size_bits = 512;               // element size (bits)
const int to_compress_size_bits = 512;        // how many of the element bits to compress
const int subarray_row = 512;                 // rows per subarray
const int subarray_col = 1024;                // bits per row (addressing granularity)
const int width = 16;                          // bits per column (data accounting only)
const int banks_used_compress = total_banks;

/* How many rows do we need to operate on to have gone through every single piece of data (round up) */
const uint64_t data_in_alloc =
    (amount_of_GiB_allocated * (1ULL << 30)) / ((data_size_bits * subarray_col * width) / 8);

/* IF we split all of these rows necessary to activate onto all possible banks we have available */
const uint64_t steps_per_bank =
    (data_in_alloc + total_banks - 1) / total_banks;

const uint64_t Bank_stride = MMIO_SIZE / total_banks;

/* how many bytes to shift the address to get the next subarray base address
   (no width necessary since width is not exposed on the columns) */
const int subarray_stride_bytes = (subarray_col * subarray_row) / 8;

/* Keep original step stride (used by your outer stepping) */
const int step_stride_bytes = (data_size_bits * subarray_col) / 8;

#define WORDS_PER_SUBARRAY ((subarray_col + 63) / 64)

/* ===================== Additional LuT keys ===================== */
const int ANDOutB1Comp = 0;
const int OROutB1Comp = 1;
const int CopyConstant0 = 2;
const int CopyConstant1 = 3;
const int ANDOutAddr = 4;
const int OROutAddr = 5;
const int DATARCAddr = 6;

/* ---------------------------- NEW LUT (direct storage) ----------------------------

Keep CH/RA/BA/subarray selection consistent with:
  addr = current_addr + bank * Bank_stride

CO=0 by using sub_base aligned to 64KiB.

Row stride within subarray must follow your addressing model:
  row_bytes = subarray_stride_bytes / subarray_row = (subarray_col/8)
With subarray_col=1024 => row_bytes=128 => ROW_SHIFT=7
*/

enum { LUT_SPECIALS = 11 };
#define SUBARRAY_ID_BITS 8u
#define SUBARRAYS_PER_KEY (1u << SUBARRAY_ID_BITS) /* 256 */

#define ROW_BYTES ((uint32_t)(subarray_stride_bytes / subarray_row)) /* == subarray_col/8 */
#define ROW_SHIFT 7u /* log2(ROW_BYTES); for your params ROW_BYTES=128 */

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

typedef struct {
  uint16_t row[LUT_SPECIALS]; /* row_in_sub: 0..subarray_row-1 */
} lut_entry_t;

static lut_entry_t *LUT; /* flat: [banks_used_compress][256] */

static inline lut_entry_t *lut_get(int bank, uint32_t sub8) {
  return &LUT[(size_t)bank * (size_t)SUBARRAYS_PER_KEY + (size_t)sub8];
}

static inline uint64_t row_off_bytes(uint16_t row_in_sub) {
  return ((uint64_t)row_in_sub) << ROW_SHIFT;
}

/* 8-bit subarray id from the 64KiB chunk index of current_addr (wrap OK) */
static inline uint32_t sub8_from_current(uint64_t current_addr) {
  uint64_t addr64k = current_addr & ~UINT64_C(0xFFFF);
  return (uint32_t)(((addr64k - MMIO_BASE) >> 16) & (SUBARRAYS_PER_KEY - 1u));
}

static inline void lut_set_key(int bank, uint32_t sub8,
                               uint16_t maj_a0, uint16_t maj_b0,
                               uint16_t maj_a1, uint16_t maj_b1,
                               uint16_t cnst_a0, uint16_t cnst_b0,
                               uint16_t cnst_a1, uint16_t cnst_b1,
                               uint16_t andor0, uint16_t andor1,
                               uint16_t datarc) {
  lut_entry_t *e = lut_get(bank, sub8);
  e->row[IDX_MAJ_A0]  = maj_a0;
  e->row[IDX_MAJ_B0]  = maj_b0;
  e->row[IDX_CNST_A0] = cnst_a0;
  e->row[IDX_CNST_B0] = cnst_b0;
  e->row[IDX_ANDOR0]  = andor0;

  e->row[IDX_MAJ_A1]  = maj_a1;
  e->row[IDX_MAJ_B1]  = maj_b1;
  e->row[IDX_CNST_A1] = cnst_a1;
  e->row[IDX_CNST_B1] = cnst_b1;
  e->row[IDX_ANDOR1]  = andor1;

  e->row[IDX_DATARC]  = datarc;
}

static void lut_populate_all(void) {
  if (!LUT) {
    LUT = (lut_entry_t *)malloc((size_t)banks_used_compress *
                                (size_t)SUBARRAYS_PER_KEY *
                                sizeof(*LUT));
  }
  if (!LUT) {
    fprintf(stderr, "LUT alloc failed\n");
    abort();
  }

  for (int bank = 0; bank < banks_used_compress; bank++) {
    for (uint32_t sub8 = 0; sub8 < SUBARRAYS_PER_KEY; sub8++) {
      /* PLACEHOLDER: all 11 special rows are row_in_sub = 0.
         Replace with real mapping values (0..subarray_row-1). */
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

  for (uint64_t step = 0; step < steps_per_bank; step++) {

    uint32_t sub8 = sub8_from_current(current_addr);

    for (int i = 0; i < to_compress_size_bits; i++) {

      uint64_t addr = current_addr;
      int lane = (i > to_compress_size_bits / 2);

      if (i == 0 || i == to_compress_size_bits / 2) {
        for (int bank = 0; bank < banks_used_compress; bank++) {
          uint64_t sub_base = addr & ~UINT64_C(0xFFFF);
          const lut_entry_t *e = lut_get(bank, sub8);

          uint16_t row_in_sub = e->row[lane ? IDX_ANDOR1 : IDX_ANDOR0];
          uint64_t andor_addr = sub_base + row_off_bytes(row_in_sub);

          rowclone(addr, andor_addr);
          addr += Bank_stride;
        }
      } else {

        if (i > 0 && i != to_compress_size_bits / 2 && (i % 32) == 0) {
          addr = current_addr;
          for (int bank = 0; bank < banks_used_compress; bank++) {
            uint64_t sub_base = addr & ~UINT64_C(0xFFFF);
            const lut_entry_t *e = lut_get(bank, sub8);

            uint16_t ra = e->row[lane ? IDX_CNST_A1 : IDX_CNST_A0];
            uint16_t rb = e->row[lane ? IDX_CNST_B1 : IDX_CNST_B0];

            uint64_t cnst_a_addr = sub_base + row_off_bytes(ra);
            uint64_t cnst_b_addr = sub_base + row_off_bytes(rb);

            rowclone(cnst_a_addr, cnst_b_addr);
            addr += Bank_stride;
          }
        }

        addr = current_addr;
        for (int bank = 0; bank < banks_used_compress; bank++) {
          uint64_t sub_base = addr & ~UINT64_C(0xFFFF);
          const lut_entry_t *e = lut_get(bank, sub8);

          uint64_t datarc_addr = sub_base + row_off_bytes(e->row[IDX_DATARC]);

          rowclone(addr, datarc_addr);
          addr += Bank_stride;
        }

        addr = current_addr;
        for (int bank = 0; bank < banks_used_compress; bank++) {
          uint64_t sub_base = addr & ~UINT64_C(0xFFFF);
          const lut_entry_t *e = lut_get(bank, sub8);

          uint16_t ra = e->row[lane ? IDX_MAJ_A1 : IDX_MAJ_A0];
          uint16_t rb = e->row[lane ? IDX_MAJ_B1 : IDX_MAJ_B0];

          uint64_t maj_a_addr = sub_base + row_off_bytes(ra);
          uint64_t maj_b_addr = sub_base + row_off_bytes(rb);

          majority(maj_a_addr, maj_b_addr);
          addr += Bank_stride;
        }
      }
    }

    current_addr += step_stride_bytes;
  }
}

int main(void) {
  lut_populate_all();
  compress_and_compare();
  return 0;
}
