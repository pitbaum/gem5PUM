#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>


#define MMIO_BASE 0x200000000ULL  // must match your Python config
#define OFSET 0x200100000ULL

static inline void Rowclone(uint64_t addr, uint64_t value)
{
    asm volatile (
        ".insn s 0x0b, 0, %1, 0(%0)\n"
        :
        : "r"(addr),
          "r"(value)
        : "memory"
    );
}

static inline void Majority(uint64_t addr, uint64_t value)
{
    asm volatile (
        ".insn s 0x0b, 1, %1, 0(%0)\n"
        :
        : "r"(addr),
          "r"(value)
        : "memory"
    );
}

int main(void)
{
    uint64_t mmio = MMIO_BASE;
    // Your custom PUM ops (still hitting the same address)
    Rowclone(mmio, 0);
    Majority(OFSET, 0);

    return 0;
}
