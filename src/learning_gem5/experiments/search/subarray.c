#include <stdint.h>

// Returns the GLOBAL physical address of the subarray start that contains `addr`.
// - window_base: base of the MMIO window (e.g., 0x200000000ULL)
// - channels   : 1 => no interleave; 2/4/8/... => low-bit striping
// - intlv_lowbit: starting bit of striping field (e.g., 6 for 64B lines)
// - rows_per_sa : rows per subarray
// - bits_per_row: row width (bits); e.g., 1024 -> 128B per row (col)
static inline uint64_t
subarray_start_global(uint64_t addr,
                      uint64_t window_base,
                      unsigned channels,
                      unsigned intlv_lowbit,
                      unsigned rows_per_sa,
                      unsigned bits_per_row)
{
    const uint64_t bytes_per_row = (uint64_t)bits_per_row / 8;
    const uint64_t bytes_per_subarray = (uint64_t)rows_per_sa * bytes_per_row;

    if (bytes_per_subarray == 0) return window_base;

    // Compute channel-local, contiguous payload offset from global addr.
    uint64_t payload;
    if (channels <= 1) {
        payload = addr - window_base;
    } else {
        const unsigned intlv_bits = __builtin_ctz(channels); // log2(channels)
        const uint64_t low_mask = (1ull << intlv_lowbit) - 1ull;

        // Split global addr into {high | interleave field | low}
        const uint64_t low = addr & low_mask;
        const uint64_t high = addr & ~((1ull << (intlv_lowbit + intlv_bits)) - 1ull);

        // Compress out the interleave field to get a contiguous payload
        payload = (high >> intlv_bits) | low;

        // Do the same transform to window_base and subtract to make it window-relative
        const uint64_t wb_low = window_base & low_mask;
        const uint64_t wb_high = window_base & ~((1ull << (intlv_lowbit + intlv_bits)) - 1ull);
        const uint64_t wb_payld = (wb_high >> intlv_bits) | wb_low;

        payload -= wb_payld;
    }

    // Align down to subarray boundary (channel-local view)
    const uint64_t payload_sa_base = (payload / bytes_per_subarray) * bytes_per_subarray;

    // Lift back to global PA
    if (channels <= 1) {
        return window_base + payload_sa_base;
    } else {
        const unsigned intlv_bits = __builtin_ctz(channels);
        const uint64_t ch = (addr >> intlv_lowbit) & ((1ull << intlv_bits) - 1ull);
        return window_base + (payload_sa_base << intlv_bits) + (ch << intlv_lowbit);
    }
}
