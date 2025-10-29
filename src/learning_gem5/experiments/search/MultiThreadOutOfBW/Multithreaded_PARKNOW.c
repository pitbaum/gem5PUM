// flat_mmio_no_interleave_pthreads.c — flat (non-interleaved) MMIO addressing
// pthreads version: exactly NUM_WORKERS threads, each owns a subset of banks
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>

typedef enum { false, true } bool;

/* ===================== Config ===================== */
#define MMIO_BASE       0x200000000ULL       // 8 GiB boundary
#define MMIO_SIZE       (16ULL << 30)        // 16 GiB window

#define CHANNELS        2
#define RANKS_PER_CH    1
#define BANKS_PER_RANK  16

/* Set the exact number of worker threads you want (e.g., 12) */
#ifndef NUM_WORKERS
#define NUM_WORKERS     8
#endif

/* User parameters */
const int      amount_of_GiB_allocated = 1;     // how much MMIO to sweep
const size_t   data_size   = sizeof(int);       // element size (bytes)
const uint64_t subarray_row = 1024;             // rows per subarray
const uint64_t subarray_col = 512;             // bits per row

/* Derived geometry */
const uint64_t ROW_BYTES        = (uint64_t)subarray_col / CHAR_BIT;     // 1024/8 = 128 B
const uint64_t SUBARRAY_BYTES   = (uint64_t)subarray_row * ROW_BYTES;    // 1024*128 = 128 KiB
const uint64_t DATA_BITS        = (uint64_t)CHAR_BIT * data_size;        // 32 for 4-byte int
const uint64_t STRIDE_BYTES     = DATA_BITS * ROW_BYTES;                  // 32 * 128 = 4096 B
const uint64_t AMOUNT_OF_SUBARRAYS = MMIO_SIZE / SUBARRAY_BYTES;

/* ================ LUT (subarray base → APA addrs) ================ */
typedef struct { uint64_t a, b; } addrpair_t;

typedef struct {
    uint64_t    base;      // start address
    uint64_t    step;      // stride between consecutive keys (bytes)
    uint64_t    nkeys;     // number of key addresses
    uint8_t     id_count;  // number of id variants per address (<= 255)
    addrpair_t *data;      // length = nkeys * id_count
} lut_t;

static inline void
lut_init(lut_t *L, uint64_t base, uint64_t nkeys, uint64_t step, uint8_t id_count)
{
    L->base = base; L->step = step; L->nkeys = nkeys; L->id_count = id_count;

    uint64_t slots = nkeys * (uint64_t)id_count;
    L->data = (addrpair_t*)malloc(slots * sizeof(addrpair_t));
    if (!L->data) { perror("malloc"); abort(); }

    for (uint64_t i = 0; i < nkeys; ++i) {
        uint64_t addr  = base + i * step;     // exact subarray base
        uint64_t row0  = i * (uint64_t)id_count;
        for (uint8_t id = 0; id < id_count; ++id) {
            L->data[row0 + id].a = addr;
            L->data[row0 + id].b = addr;
        }
    }
}

static inline addrpair_t
lut_lookup(const lut_t *L, uint64_t addr, uint8_t id)
{
    uint64_t i   = (addr - L->base) / L->step;
    uint64_t idx = i * (uint64_t)L->id_count + id;
    return L->data[idx];
}

/* ================= PuM stubs (instrumented opcodes) ================= */
static inline void rowclone(uintptr_t target_addr){
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    asm volatile("xor %%rax, %%rax" ::: "rax");
}

static inline void majority(uintptr_t target_addr){
    asm volatile(
        "mov %0, %%rax\n\t"
        ".byte 0x15, 0x28\n\t"
        :: "r"(target_addr)
        : "rax", "eax","memory"
    );
    asm volatile("xor %%rax, %%rax" ::: "rax");
}

/* LUT identifiers used in this variant */
enum { ANDOutB1C0 = 0, OROutB1C1 = 1 };

/* One PuM compare for a given subarray; called per (bank, step) */
static inline void
pum_similarity_check(uint64_t subarray_addr, const lut_t *L, uint64_t subarray_identifier)
{
    for (int i = 0; i < (int)data_size; i++){
        /* Copy 0/1 row */
        rowclone(subarray_addr);
        rowclone(subarray_addr);

        /* Copy Di row */
        rowclone(subarray_addr);
        rowclone(subarray_addr);

        /* Choose AND vs OR half/half */
        addrpair_t APA = (i < (int)(data_size/2))
                         ? lut_lookup(L, subarray_identifier, ANDOutB1C0)
                         : lut_lookup(L, subarray_identifier, OROutB1C1);

        majority(APA.a);
        majority(APA.b);

        /* Seed Out rows on first items of each half */
        if (i == 0 || i == (int)(data_size/2)) {
            rowclone(subarray_addr);
            rowclone(subarray_addr);
        }
    }
}

/* Simple readback predicate (placeholder) */
static inline bool a1_b0_any(volatile const uint8_t *base) {
    const volatile uint64_t *a = (const volatile uint64_t *)(base);
    const volatile uint64_t *b = (const volatile uint64_t *)(base + ROW_BYTES);
#pragma GCC unroll 8
    for (int i = 0; i < (int)(ROW_BYTES / 8); ++i) {
        uint64_t av = a[i];
        uint64_t bv = b[i];
        (void)(av & ~bv);
    }
    return false;
}

/* ===================== Worker threading ===================== */
typedef struct {
    int              tid;                 // 0..NUM_WORKERS-1
    const lut_t     *L;                   // shared LUT
    const uint64_t  *bank_base0;          // [total_banks] base addr at step=0 (per bank)
    uint64_t        *sub_id;              // [total_banks] subarray identifiers (updated on boundaries)
    uint64_t         steps;
    uint64_t         stride_bytes;
    uint64_t         steps_per_subarray;
    uint64_t         total_banks;
} worker_arg_t;

static void* worker_main(void *vp)
{
    worker_arg_t *wa = (worker_arg_t*)vp;

    for (uint64_t step = 0; step < wa->steps; ++step) {
        const uint64_t off = step * wa->stride_bytes;
        const bool at_boundary = (wa->steps_per_subarray != 0)
                               && ((step % wa->steps_per_subarray) == 0);

        // This worker handles banks: tid, tid+NUM_WORKERS, tid+2*NUM_WORKERS, ...
        for (uint64_t b = (uint64_t)wa->tid; b < wa->total_banks; b += (uint64_t)NUM_WORKERS) {
            const uint64_t addr = wa->bank_base0[b] + off;

            if (at_boundary)
                wa->sub_id[b] = addr;  // update this bank's current subarray base

            pum_similarity_check(addr, wa->L, wa->sub_id[b]);
            (void)a1_b0_any((volatile const uint8_t*)(uintptr_t)addr);
        }
    }
    return NULL;
}

/* ===================== Driver ===================== */
uint64_t parallel_access(const lut_t *L)
{
    const uint64_t per_ch_size       = MMIO_SIZE / CHANNELS;              // 8 GiB
    const uint64_t banks_per_ch      = (uint64_t)RANKS_PER_CH * BANKS_PER_RANK; // 16
    const uint64_t total_banks       = (uint64_t)CHANNELS * banks_per_ch; // 32

    /* Channel base addresses */
    uint64_t base_ch[CHANNELS];
    for (unsigned ch = 0; ch < CHANNELS; ++ch)
        base_ch[ch] = MMIO_BASE + (uint64_t)ch * per_ch_size;

    /* Bank slice within each channel */
    const uint64_t bank_region_bytes = per_ch_size / banks_per_ch;        // 512 MiB
    uint64_t bank_off_in_ch[banks_per_ch];
    for (unsigned rk = 0; rk < RANKS_PER_CH; ++rk)
        for (unsigned bk = 0; bk < BANKS_PER_RANK; ++bk)
            bank_off_in_ch[rk * BANKS_PER_RANK + bk] =
                ((uint64_t)rk * BANKS_PER_RANK + bk) * bank_region_bytes;

    /* Precompute base address at step 0 for each flat bank index */
    uint64_t *bank_base0 = (uint64_t*)malloc(total_banks * sizeof(uint64_t));
    if (!bank_base0) { perror("malloc bank_base0"); abort(); }

    for (uint64_t b = 0; b < total_banks; ++b) {
        const uint64_t ch = b / banks_per_ch;
        const uint64_t rem = b % banks_per_ch;
        bank_base0[b] = base_ch[ch] + bank_off_in_ch[rem];
    }

    /* Step limits */
    const uint64_t stride_bytes       = STRIDE_BYTES;                     // 4 KiB
    const uint64_t max_steps_per_bank = bank_region_bytes / stride_bytes; // 512 MiB / 4 KiB
    const uint64_t bytes_per_step_all = stride_bytes * total_banks;       // 4 KiB * 32 = 128 KiB
    const uint64_t alloc_bytes_total  = ((uint64_t)amount_of_GiB_allocated) << 30;
    const uint64_t steps_by_alloc     = (alloc_bytes_total + bytes_per_step_all - 1) / bytes_per_step_all;
    const uint64_t steps              = (steps_by_alloc < max_steps_per_bank)
                                      ? steps_by_alloc : max_steps_per_bank;

    const uint64_t steps_per_subarray = subarray_row / DATA_BITS;         // 1024 / 32 = 32

    /* Subarray identifiers (one per bank), init to step0 addresses */
    uint64_t *sub_id = (uint64_t*)malloc(total_banks * sizeof(uint64_t));
    if (!sub_id) { perror("malloc sub_id"); abort(); }
    for (uint64_t b = 0; b < total_banks; ++b) sub_id[b] = bank_base0[b];

    /* Spawn exactly NUM_WORKERS threads */
    pthread_t thr[NUM_WORKERS];
    worker_arg_t args[NUM_WORKERS];
    for (int t = 0; t < NUM_WORKERS; ++t) {
        args[t].tid               = t;
        args[t].L                 = L;
        args[t].bank_base0        = bank_base0;
        args[t].sub_id            = sub_id;
        args[t].steps             = steps;
        args[t].stride_bytes      = stride_bytes;
        args[t].steps_per_subarray= steps_per_subarray;
        args[t].total_banks       = total_banks;
        if (pthread_create(&thr[t], NULL, worker_main, &args[t]) != 0) {
            perror("pthread_create");
            abort();
        }
    }
    for (int t = 0; t < NUM_WORKERS; ++t) pthread_join(thr[t], NULL);

    /* Optional: print a quick summary to sanity-check progress */
    printf("Done. workers=%d, banks=%" PRIu64 ", steps=%" PRIu64
           ", stride=%" PRIu64 " B (%.1f KiB)\n",
           NUM_WORKERS, total_banks, steps, stride_bytes,
           (double)stride_bytes / 1024.0);

    free(bank_base0);
    free(sub_id);
    return 0;
}

/* ===================== main ===================== */
int main(void)
{
    /* LUT over subarray bases: keys spaced by SUBARRAY_BYTES */
    lut_t L;
    lut_init(&L, MMIO_BASE, AMOUNT_OF_SUBARRAYS, SUBARRAY_BYTES, 4);

    parallel_access(&L);

    printf("Scan completed: successfully.\n");
    free(L.data);
    return 0;
}
