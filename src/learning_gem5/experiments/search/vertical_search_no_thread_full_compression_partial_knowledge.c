// flat_mmio_no_interleave.c — flat (non-interleaved) MMIO addressing
// Using no additional time for base row copies, using the partial knowledge and compress only data not base
// Obviously not for trivial case
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>

typedef enum { false, true } bool;

#define MMIO_BASE       0x200000000ULL       // 8 GiB boundary
#define MMIO_SIZE       (16ULL << 30)        // 16 GiB window

#define CHANNELS        2
#define RANKS_PER_CH    1
#define BANKS_PER_RANK  16

// User parameters
const int      amount_of_GiB_allocated = 1;  // how much of MMIO you plan to use
const size_t   data_size   = sizeof(int);    // element size (bytes)
const uint64_t subarray_row = 1024;          // rows per subarray
const uint64_t subarray_col = 512;           // bits per row

// Derived geometry
const uint64_t ROW_BYTES        = (uint64_t)subarray_col / CHAR_BIT;
const uint64_t SUBARRAY_BYTES   = (uint64_t)subarray_row * ROW_BYTES;
const uint64_t DATA_BITS        = (uint64_t)CHAR_BIT * data_size;
const uint64_t STRIDE_BYTES     = DATA_BITS * ROW_BYTES;
const uint64_t AMOUNT_OF_SUBARRAYS = MMIO_SIZE / SUBARRAY_BYTES;

// ---------------- LUT ----------------
typedef struct { uint64_t a, b; } addrpair_t;

typedef struct {
    uint64_t    base;      // start address
    uint64_t    step;      // stride between consecutive keys (bytes)
    uint64_t    nkeys;     // number of key addresses
    uint8_t     id_count;  // number of id variants per address (<= 255)
    addrpair_t *data;      // length = nkeys * id_count
} lut_t;

// Init the look upt table to go from subarray + id to APA addresses
static inline void
lut_init(lut_t *L, uint64_t base, uint64_t nkeys, uint64_t step, uint8_t id_count)
{
    L->base = base; L->step = step; L->nkeys = nkeys; L->id_count = id_count;

    uint64_t slots = nkeys * (uint64_t)id_count;
    L->data = (addrpair_t*)malloc(slots * sizeof(addrpair_t));
    if (!L->data) abort();

    for (uint64_t i = 0; i < nkeys; ++i) {
        uint64_t addr  = base + i * step;           // exact key address
        uint64_t row0  = i * (uint64_t)id_count;
        for (uint8_t id = 0; id < id_count; ++id) {
            L->data[row0 + id].a = addr;
            L->data[row0 + id].b = addr;
        }
    }
}

// Helper function to build the LUT
static inline addrpair_t
lut_lookup(const lut_t *L, uint64_t addr, uint8_t id)
{
    uint64_t i   = (addr - L->base) / L->step;
    uint64_t idx = i * (uint64_t)L->id_count + id;
    return L->data[idx];
}

// ---------------- PuM stubs ----------------
// Sending a RC command to memory with and address
static inline void rowclone(uintptr_t target_addr){
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    asm volatile("xor %%rax, %%rax" ::: "rax");
}

// Sending a MAJ command to memory with an address
static inline void majority(uintptr_t target_addr){
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    asm volatile("xor %%rax, %%rax" ::: "rax");
}


// Look up table identifiers
// MAJ3 ANDOut, CompB1, Comp0
const uint8_t ANDOutB1C0 = 0;
// MAJ3 OROut, CompB1, Comp1
const uint8_t OROutB1C1 = 1;

// Function that checks similarity using custom PuM row operations
// Workflow to get the results that the CPU can evaluate given the CPU knows the base data, it can AND all indexes that should be 1 and OR all indexes that should be 0
void pum_similarity_check(uint64_t subarray_addr, const lut_t *L, uint64_t subarray_identifier) {
    // For the worst case experiment, we will do 50/50 split of 0s and 1s in the base data
    // Keep the operation up until the end of the data entry size
    for (int i=0; i < data_size; i++){
        // Copy 0 or 1 into the computational row
        rowclone(subarray_addr);
        rowclone(subarray_addr);
        
        // Copy Di into the computational row
        rowclone(subarray_addr);
        rowclone(subarray_addr);

        // MAJ3 CompB1, CompD1, Comp0 (AND)
        // look up time for the MMU addr translation to APA
        addrpair_t APA;
        if (i<data_size/2)
            APA = lut_lookup(L, subarray_identifier, ANDOutB1C0);    
        else
            APA = lut_lookup(L, subarray_identifier, OROutB1C1);
        
        majority(APA.a);
        majority(APA.b);

        // First iteration for 1s and 0s only copy into the respective Outs
        if(i == 0 || i==data_size/2)
        {
            // RC 1st index expected to be 0 or 1 into ANDout or ORout
            rowclone(subarray_addr);
            rowclone(subarray_addr);
        }
    }
}

static inline uint64_t ceil_div_u64(uint64_t a, uint64_t b) {
    return a / b + ((a % b) != 0);
}

static inline bool a1_b0_any(volatile const uint8_t *base) {
    const volatile uint64_t *a = (const volatile uint64_t *)(base);
    const volatile uint64_t *b = (const volatile uint64_t *)(base + ROW_BYTES);

    // ROW_BYTES == 64 -> 8 x 8B
#pragma GCC unroll 8
    for (int i = 0; i < (int)(ROW_BYTES / 8); ++i) {
        uint64_t av = a[i];
        uint64_t bv = b[i];
        (av & ~bv);
    }
    return false;
}

uint64_t parallel_access(const lut_t *L)
{
    // Per-channel contiguous slice (flat)
    const uint64_t per_ch_size      = MMIO_SIZE / CHANNELS;                 // 16 GiB / 2 = 8 GiB
    const uint64_t banks_per_ch     = (uint64_t)RANKS_PER_CH * BANKS_PER_RANK; // 16

    // Precompute channel bases
    uint64_t base_ch[CHANNELS];
    for (unsigned ch = 0; ch < CHANNELS; ++ch)
        base_ch[ch] = MMIO_BASE + (uint64_t)ch * per_ch_size;

    // Bank slice within a channel
    const uint64_t bank_region_bytes = per_ch_size / banks_per_ch;
    uint64_t bank_off[RANKS_PER_CH * BANKS_PER_RANK];
    for (unsigned rk = 0; rk < RANKS_PER_CH; ++rk)
        for (unsigned bk = 0; bk < BANKS_PER_RANK; ++bk)
            bank_off[rk * BANKS_PER_RANK + bk] =
                ((uint64_t)rk * BANKS_PER_RANK + bk) * bank_region_bytes;

    // Stride & step caps
    const uint64_t stride_bytes       = STRIDE_BYTES;                        // from globals
    const uint64_t max_steps_per_bank = bank_region_bytes / stride_bytes;

    const uint64_t total_banks        = (uint64_t)CHANNELS * banks_per_ch;  // 32
    const uint64_t bytes_per_step_all = stride_bytes * total_banks;         // processed per outer step
    const uint64_t alloc_bytes_total  = ((uint64_t)amount_of_GiB_allocated) << 30;

    const uint64_t max_steps_by_alloc = ceil_div_u64(alloc_bytes_total, bytes_per_step_all);
    const uint64_t steps              = (max_steps_by_alloc < max_steps_per_bank)
                                      ? max_steps_by_alloc : max_steps_per_bank;

    // New-subarray boundary every (subarray_row / DATA_BITS) steps (uses globals)
    const uint64_t steps_per_subarray = subarray_row / DATA_BITS;

    // One identifier per bank
    uint64_t subarray_identifier[CHANNELS * RANKS_PER_CH * BANKS_PER_RANK];
    for (size_t i = 0; i < sizeof subarray_identifier / sizeof subarray_identifier[0]; ++i)
        subarray_identifier[i] = 0;

    for (uint64_t step = 0; step < steps; ++step) {
        const uint64_t off = step * stride_bytes;
        const int at_sub_boundary = ((steps_per_subarray != 0) && (step % steps_per_subarray == 0));

        for (unsigned ch = 0; ch < CHANNELS; ++ch) {
            const uint64_t base = base_ch[ch];
            for (unsigned rk = 0; rk < RANKS_PER_CH; ++rk) {
                for (unsigned bk = 0; bk < BANKS_PER_RANK; ++bk) {

                    const uint64_t addr = base + bank_off[rk * BANKS_PER_RANK + bk] + off;

                    const size_t key_idx =
                        (size_t)ch * (RANKS_PER_CH * BANKS_PER_RANK)
                      + (size_t)rk * BANKS_PER_RANK + bk;

                    if (at_sub_boundary)
                        subarray_identifier[key_idx] = addr;
                    
                    // print every step
                    /*    
                    printf("step%-6" PRIu64 " ch%u rk%u bk%u -> 0x%016" PRIx64
                           " , ident 0x%016" PRIx64 "\n",
                           step, ch, rk, bk, addr, subarray_identifier[key_idx]);
                    */

                    // Issue PUM computations
                    pum_similarity_check(addr, L, subarray_identifier[key_idx]);

                    // read out and evaluate
                    a1_b0_any((volatile const uint8_t*)(uintptr_t)addr);
                    // technically we should return if this is true the address we currently prcessed
                    // Since the PUM host address space is stubbed and we do a full range computation we dont care
                    // Dont expect an early hit
                }
            }
        }
    }

    // summary:
    //printf("Done. steps=%" PRIu64 ", stride=0x%" PRIx64 " (%" PRIu64 " B), alloc=%" PRIu64 " GiB\n", steps, stride_bytes, stride_bytes, (uint64_t)amount_of_GiB_allocated);
    return 0;
}

// ---------------- main ----------------
int main(void)
{
    // LUT over subarray bases
    lut_t L;
    lut_init(&L,MMIO_BASE,AMOUNT_OF_SUBARRAYS,SUBARRAY_BYTES,2);

    parallel_access(&L);

    printf("Scan completed: successfully.\n");
    return 0;
}
