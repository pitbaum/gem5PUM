// flat_stride_lut.h/.c — minimal (addr, id8) -> {addr, addr}
#include <stdint.h>
#include <stdlib.h>

typedef struct { uint64_t a, b; } addrpair_t;

typedef struct {
    uint64_t    base;      // start address
    uint64_t    step;      // stride between consecutive keys
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

static inline void lut_print_keys(const lut_t *L)
{
    for (uint64_t i = 0; i < L->nkeys; ++i) {
        uint64_t addr = L->base + i * L->step;
        for (uint8_t id = 0; id < L->id_count; ++id) {
            printf("%#018" PRIx64 " %u\n", addr, (unsigned)id);
        }
    }
}

#include <inttypes.h>  // for PRIu64
#include <stdio.h>

// Print how big the LUT is (counts and memory footprint)
static inline void lut_print_stats(const lut_t *L)
{
    uint64_t slots = L->nkeys * (uint64_t)L->id_count;          // total entries
    uint64_t bytes = slots * (uint64_t)sizeof(addrpair_t);      // heap bytes for L->data
    uint64_t header = (uint64_t)sizeof(*L);                     // struct itself

    printf("LUT stats:\n");
    printf("  base      = %#018" PRIx64 "\n", L->base);
    printf("  nkeys     = %" PRIu64 "\n", L->nkeys);
    printf("  id_count  = %u\n", (unsigned)L->id_count);
    printf("  slots     = %" PRIu64 " (nkeys * id_count)\n", slots);
    printf("  entry size= %zu bytes (addrpair_t)\n", sizeof(addrpair_t));
    printf("  heap size = %" PRIu64 " bytes (%.2f MiB)\n",
           bytes, (double)bytes / (1024.0*1024.0));
    printf("  header    = %" PRIu64 " bytes (sizeof(lut_t))\n", header);
    printf("  total est = %" PRIu64 " bytes (%.2f MiB)\n",
           bytes + header, (double)(bytes + header) / (1024.0*1024.0));

    // quick sanity: last key address
    if (L->nkeys) {
        uint64_t last_key = L->base + (L->nkeys - 1) * L->step;
        printf("  key[0]   = %#018" PRIx64 "\n", L->base);
        printf("  key[last]= %#018" PRIx64 " (step=%" PRIu64 ")\n", last_key, L->step);
    }
}
