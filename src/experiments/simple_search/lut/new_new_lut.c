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

const uint64_t steps_per_bank =
    (data_in_alloc + total_banks - 1) / total_banks;

const uint64_t Bank_stride = MMIO_SIZE / total_banks;

const int subarray_stride_bytes = (subarray_col * subarray_row) / 8;
const int step_stride_bytes = (data_size_bits * subarray_col) / 8;

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

/* 16-bit offset lane: lane[0] and lane[1] are always selected together */
typedef struct {
  uint16_t maj_a, maj_b;
  uint16_t cnst_a, cnst_b;
  uint16_t andor;
} lane16_t;

typedef struct {
  lane16_t lane[LUT_OPS]; /* lane[0..1] */
  uint16_t datarc;
  uint16_t _pad;          /* optional */
} lut16_t;

static lut16_t *LUT;

/* Populate one cell: (sub, lane[0|1]) -> (a,b) offsets */
static inline void lut_set_sub(uint64_t sub_id, uint16_t a, uint16_t b) {
  LUT[sub_id].lane[0].maj_a  = a;  LUT[sub_id].lane[0].maj_b  = b;
  LUT[sub_id].lane[1].maj_a  = a;  LUT[sub_id].lane[1].maj_b  = b;

  LUT[sub_id].lane[0].cnst_a = a;  LUT[sub_id].lane[0].cnst_b = b;
  LUT[sub_id].lane[1].cnst_a = a;  LUT[sub_id].lane[1].cnst_b = b;

  LUT[sub_id].lane[0].andor  = a;
  LUT[sub_id].lane[1].andor  = b;

  LUT[sub_id].datarc         = a;
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

    uint16_t a = 0;
    uint16_t b = 0;
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
  const lut16_t *lutp[banks_used_compress]; /* NEW: per-bank LUT pointer */

  for (uint64_t step = 0; step < steps_per_bank; step++) {

    uint64_t base = current_addr;
    for (int bank = 0; bank < banks_used_compress; bank++) {
        uint64_t addr = base + (uint64_t)bank * Bank_stride;
        uint64_t addr64k = addr & ~UINT64_C(0xFFFF);

        sub_base[bank]  = addr64k;
        sub_id[bank]    = (uint32_t)((addr64k - MMIO_BASE) >> 16);

        lutp[bank] = &LUT[sub_id[bank]]; /* NEW */
    }

    for (int i = 0; i < to_compress_size_bits; i++) {

      int lane = (i > 256);

      if (i == 0 || i == 256) {
        for (int bank = 0; bank < banks_used_compress; bank++) {
          const lane16_t *ln = &lutp[bank]->lane[lane];
          acc ^= sub_base[bank] + (uint64_t)ln->andor;
        }

      } else {

        if ((i & 31) == 0) { /* NEW: cheaper than i % 32 */
          for (int bank = 0; bank < banks_used_compress; bank++) {
            const lane16_t *ln = &lutp[bank]->lane[lane];
            acc ^= sub_base[bank] + (uint64_t)ln->cnst_a;
            acc ^= sub_base[bank] + (uint64_t)ln->cnst_b;
          }
        }

        for (int bank = 0; bank < banks_used_compress; bank++) {
          acc ^= sub_base[bank] + (uint64_t)lutp[bank]->datarc;
        }

        for (int bank = 0; bank < banks_used_compress; bank++) {
          const lane16_t *ln = &lutp[bank]->lane[lane];
          acc ^= sub_base[bank] + (uint64_t)ln->maj_a;
          acc ^= sub_base[bank] + (uint64_t)ln->maj_b;
        }
      }
    }

    current_addr += step_stride_bytes;
  }
  sink = acc;
}

int main(void) {
  lut_populate_all();
  compress_and_compare();
  return 0;
}
