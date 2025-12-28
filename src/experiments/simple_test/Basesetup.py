from m5.objects import (
    AddrRange,
    Cache,
    DerivO3CPU,
    L2XBar,
    LocalBP,
    Process,
    Ramulator2,
    Root,
    SEWorkload,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
)
from m5.util.convert import toMemorySize
from m5.objects import TAGE_SC_L_64KB

# --------- Classic caches ---------
class L1ICache(Cache):
    def __init__(self, size="64KiB", assoc=8):
        super().__init__()
        self.size = size
        self.assoc = assoc
        self.tag_latency = 2
        self.data_latency = 2
        self.response_latency = 2
        self.mshrs = 16
        self.tgts_per_mshr = 8


class L1DCache(Cache):
    def __init__(self, size="64KiB", assoc=8):
        super().__init__()
        self.size = size
        self.assoc = assoc
        self.tag_latency = 2
        self.data_latency = 2
        self.response_latency = 2
        self.mshrs = 16
        self.tgts_per_mshr = 8


class L2Cache(Cache):
    def __init__(self, size="512KiB", assoc=16):
        super().__init__()
        self.size = size
        self.assoc = assoc
        self.tag_latency = 12
        self.data_latency = 12
        self.response_latency = 12
        self.mshrs = 64
        self.tgts_per_mshr = 16


# --------- build system ---------
system = System()
system.mem_mode = "timing"
system.clk_domain = SrcClockDomain(
    clock="4GHz", voltage_domain=VoltageDomain()
)

# buses
system.membus = SystemXBar(width=64)
system.l2bus = L2XBar()

# --------- 1 O3 CPU (RISC-V build of gem5) ---------
system.cpu = DerivO3CPU()
system.cpu.numThreads = 1
system.cpu.branchPred = TAGE_SC_L_64KB()

# caches
system.icache = L1ICache()
system.dcache = L1DCache()
system.l2cache = L2Cache()

# ---------- Address map ----------
MAIN_SIZE = "8GiB"
MMIO_BASE = 0x2_0000_0000  # 8 GiB boundary
MMIO_SIZE = "64GiB"        # size of the MMIO window

main_range = AddrRange(0, size=MAIN_SIZE)
mmio_size_bytes = toMemorySize(MMIO_SIZE)
mmio_range = AddrRange(MMIO_BASE, size=mmio_size_bytes)

# Publish memory ranges (logical view)
system.mem_ranges = [main_range, mmio_range]

# ---------- I-side & D-side: through caches (no MMIO bypass) ----------
system.cpu.icache_port = system.icache.cpu_side
system.cpu.dcache_port = system.dcache.cpu_side

# ---------- L2 <-> membus ----------
system.l2bus.cpu_side_ports = [
    system.icache.mem_side,
    system.dcache.mem_side,
]

system.l2cache.cpu_side = system.l2bus.mem_side_ports
system.l2cache.mem_side = system.membus.cpu_side_ports

# ---------- Walkers (for MMU) ----------
if hasattr(system.cpu, "itb") and hasattr(system.cpu.itb, "walk_port"):
    system.cpu.itb.walk_port = system.membus.cpu_side_ports
if hasattr(system.cpu, "dtb") and hasattr(system.cpu.dtb, "walk_port"):
    system.cpu.dtb.walk_port = system.membus.cpu_side_ports

# RISC-V SE doesn't need anything special
system.cpu.createInterruptController()


# system port
system.system_port = system.membus.cpu_side_ports

# --------- Main memory controller (cached DRAM) ---------
system.ram_main = Ramulator2(
    config_path="ext/ramulator2/ramulator2/test_configs/base_ram.yaml"
)
system.ram_main.range = main_range

# --------- Uncached MMIO DRAM controller (no interleaving) ---------
system.ram_mmio = Ramulator2(
    config_path="ext/ramulator2/ramulator2/test_configs/4rk_4to2_pum.yaml"
)

system.ram_mmio.range = mmio_range
system.ram_mmio.null = True  # We dont need actual data storage

# Connect both memory controllers to membus
system.membus.mem_side_ports = [
    system.ram_main.port,
    system.ram_mmio.port,
]

# --------- SE workload (RISC-V binary) ---------
# riscv64-linux-gnu-gcc -O2 -Wall -march=rv64gc -static riscvtest.c -o basetest
binary = "src/experiments/simple_test/test"
system.workload = SEWorkload.init_compatible(binary)

process = Process()
process.cmd = [binary]
system.cpu.workload = process
system.cpu.createThreads()

# --------- instantiate first, then map ---------
root = Root(full_system=False, system=system)

import m5

m5.instantiate()

# VA==PA map for the MMIO window (non-cacheable)
process.map(MMIO_BASE, MMIO_BASE, mmio_size_bytes, cacheable=False)

print("Beginning simulation!")
event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {event.getCause()}")
