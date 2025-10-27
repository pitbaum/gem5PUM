#include <stdint.h>
#include <stdio.h>
#include <assert.h>

#define MMIO_BASE       0x200000000ULL       // from your config (8 GiB)
#define MMIO_SIZE       (16ULL << 30)        // 16 GiB window

#define CHANNELS        2                    // CHANNELS_MMIO
#define LINE_BYTES      64
#define INTLV_LOWBIT    6                    // log2(LINE_BYTES)
#define INTLV_BITS      1                    // log2(CHANNELS)

// <<< set these to match your Ramulator2 YAML >>>
#define RANKS_PER_CH    1
#define BANKS_PER_RANK  16
// <<< ------------------------------------------------ >>>

static inline uint64_t
insert_channel(uint64_t ch_local_bytes, unsigned ch)
{
    // Because ch_local_bytes is 64B-aligned, we can use the fast form:
    // global = MMIO_BASE + (ch_local_bytes << INTLV_BITS) + (ch << INTLV_LOWBIT)
    return MMIO_BASE
         + (ch_local_bytes << INTLV_BITS)
         + ((uint64_t)ch << INTLV_LOWBIT);
}

int main(void)
{
    // sanity
    assert((MMIO_SIZE % CHANNELS) == 0);

    const uint64_t per_ch_size      = MMIO_SIZE / CHANNELS;
    const uint64_t banks_per_ch     = (uint64_t)RANKS_PER_CH * BANKS_PER_RANK;
    assert((per_ch_size / banks_per_ch) >= LINE_BYTES);

    // make each bank region start on a 64B boundary (required for the fast insert)
    const uint64_t bank_region_bytes_unaligned = per_ch_size / banks_per_ch;
    const uint64_t bank_region_bytes =
        (bank_region_bytes_unaligned / LINE_BYTES) * LINE_BYTES;

    // enumerate first address per (ch, rank, bank)
    for (unsigned ch = 0; ch < CHANNELS; ++ch) {
        for (unsigned rk = 0; rk < RANKS_PER_CH; ++rk) {
            for (unsigned bk = 0; bk < BANKS_PER_RANK; ++bk) {

                const uint64_t idx_in_ch = (uint64_t)rk * BANKS_PER_RANK + bk;
                const uint64_t ch_local  = idx_in_ch * bank_region_bytes; // 64B-aligned
                const uint64_t addr      = insert_channel(ch_local, ch);

                printf("ch%u rk%u bk%u -> first_addr = 0x%016llx\n",
                       ch, rk, bk, (unsigned long long)addr);
            }
        }
    }
    return 0;
}
