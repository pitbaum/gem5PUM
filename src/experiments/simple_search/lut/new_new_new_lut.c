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
const int subarray_col = 1024;                // columns per row
const int width = 8;                         // bits per column
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


const uint64_t subarrays_total = MMIO_SIZE / (uint64_t)subarray_stride_bytes;

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

/* ---------------------------- LUT ------------------------------------- */
enum { LUT_OPS = 2 };

/*
 * Packed lane:
 * packed0 layout (low->high bits): maj_a | maj_b | cnst_a | cnst_b  (each 16-bit)
 * This encourages compilers to fetch 4 fields with one 64-bit load.
 */
typedef struct {
  uint64_t packed0;  // maj_a | maj_b | cnst_a | cnst_b (4x16)
  uint16_t andor;
  uint16_t _pad;
} lane16_t;

typedef struct {
  lane16_t lane[LUT_OPS]; /* lane[0..1] */
  uint16_t datarc;
  uint16_t _pad;
} lut16_t;

static lut16_t *LUT;

/* Helper to build packed0 from 4x16-bit values */
static inline uint64_t pack4x16(uint16_t maj_a, uint16_t maj_b,
                                uint16_t cnst_a, uint16_t cnst_b) {
  return ((uint64_t)maj_a) |
         ((uint64_t)maj_b  << 16) |
         ((uint64_t)cnst_a << 32) |
         ((uint64_t)cnst_b << 48);
}

/* Populate one cell: (sub, lane[0|1]) -> offsets */
static inline void lut_set_sub(uint64_t sub_id, uint16_t row_offset1, uint16_t row_offset2) {
  /* Placeholder mapping preserved:
     - maj_a/maj_b use (a,b)
     - cnst_a/cnst_b use (a,b)
     - lane[0].andor = a, lane[1].andor = b
     - datarc = a
  */
  LUT[sub_id].lane[0].packed0 = pack4x16(row_offset1, row_offset2, row_offset1, row_offset2);
  LUT[sub_id].lane[1].packed0 = pack4x16(row_offset1, row_offset2, row_offset2, row_offset1);

  LUT[sub_id].lane[0].andor = row_offset1;
  LUT[sub_id].lane[1].andor = row_offset2;

  LUT[sub_id].datarc = row_offset1;
}

// Populate all with subarray base address (placeholder offsets)
static void lut_populate_all(void) {
  if (!LUT) LUT = (lut16_t *)malloc(subarrays_total * sizeof(*LUT));

  if (!LUT) {
    fprintf(stderr, "LUT alloc failed\n");
    abort();
  }

  for (uint64_t sub = 0; sub < subarrays_total; sub++) {
    uint64_t sub_base = MMIO_BASE + (sub * (uint64_t)subarray_stride_bytes);
    (void)sub_base;

    uint16_t a = sub_base;
    uint16_t b = sub_base;
    lut_set_sub(sub, a, b);
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

  uint32_t sub_id[banks_used_compress];
  uint64_t sub_base[banks_used_compress];
  const lut16_t  *lutp[banks_used_compress];

  /* Apply (2): precompute lane pointers once per step */
  const lane16_t *lane0[banks_used_compress];
  const lane16_t *lane1[banks_used_compress];

  for (uint64_t step = 0; step < steps_per_bank; step++) {

    uint64_t base = current_addr;
    for (int bank = 0; bank < banks_used_compress; bank++) {
        uint64_t addr = base + (uint64_t)bank * Bank_stride;
        uint64_t addr64k = addr & ~UINT64_C(0xFFFF);

        sub_base[bank] = addr64k;
        sub_id[bank]   = (uint32_t)((addr64k - MMIO_BASE) >> 16);

        const lut16_t *p = &LUT[sub_id[bank]];
        lutp[bank] = p;
        lane0[bank] = &p->lane[0];
        lane1[bank] = &p->lane[1];
    }

    for (int i = 0; i < to_compress_size_bits; i++) {

      int lane = (i > 256);

      if (i == 0 || i == 256) {
        for (int bank = 0; bank < banks_used_compress; bank++) {
          const lane16_t *ln = lane ? lane1[bank] : lane0[bank];
          acc ^= sub_base[bank] + (uint64_t)ln->andor;
        }

      } else {

        if ((i & 31) == 0) {
          for (int bank = 0; bank < banks_used_compress; bank++) {
            const lane16_t *ln = lane ? lane1[bank] : lane0[bank];

            /* Apply (1): load packed0 once, extract from local */
            uint64_t p0 = ln->packed0;
            acc ^= sub_base[bank] + (uint64_t)(uint16_t)(p0 >> 32);  // cnst_a
            acc ^= sub_base[bank] + (uint64_t)(uint16_t)(p0 >> 48);  // cnst_b
          }
        }

        for (int bank = 0; bank < banks_used_compress; bank++) {
          acc ^= sub_base[bank] + (uint64_t)lutp[bank]->datarc;
        }

        for (int bank = 0; bank < banks_used_compress; bank++) {
          const lane16_t *ln = lane ? lane1[bank] : lane0[bank];

          /* Apply (1): load packed0 once, extract from local */
          uint64_t p0 = ln->packed0;
          acc ^= sub_base[bank] + (uint64_t)(uint16_t)(p0);        // maj_a
          acc ^= sub_base[bank] + (uint64_t)(uint16_t)(p0 >> 16);  // maj_b
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
