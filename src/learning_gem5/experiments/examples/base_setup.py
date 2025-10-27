from m5.objects import (
    AddrRange,
    Bridge,
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


# --------- tiny cache helpers (classic) ---------
class L1ICache(Cache):
    def __init__(self, size="32KiB", assoc=8):
        super().__init__()
        self.size = size
        self.assoc = assoc
        self.tag_latency = 2
        self.data_latency = 2
        self.response_latency = 2
        self.mshrs = 16
        self.tgts_per_mshr = 8


class L1DCache(Cache):
    def __init__(self, size="32KiB", assoc=8):
        super().__init__()
        self.size = size
        self.assoc = assoc
        self.tag_latency = 2
        self.data_latency = 2
        self.response_latency = 2
        self.mshrs = 16
        self.tgts_per_mshr = 8


class L2Cache(Cache):
    def __init__(self, size="256KiB", assoc=16):
        super().__init__()
        self.size = size
        self.assoc = assoc
        self.tag_latency = 12
        self.data_latency = 12
        self.response_latency = 12
        self.mshrs = 64
        self.tgts_per_mshr = 16


# --------- build system (no boards/components) ---------
system = System()
system.mem_mode = "timing"
system.clk_domain = SrcClockDomain(
    clock="3GHz", voltage_domain=VoltageDomain()
)

# buses
system.membus = SystemXBar(width=64)
system.l2bus = L2XBar()

# O3 CPU (single thread)
system.cpu = DerivO3CPU()
system.cpu.numThreads = 1
system.cpu.branchPred = LocalBP()

# caches
system.icache = L1ICache()
system.dcache = L1DCache()
system.l2cache = L2Cache()

# ---------- Address map ----------
MAIN_SIZE = "8GB"
MMIO_BASE = 0x2_0000_0000  # 8 GiB
MMIO_SIZE = "8GB"

main_range = AddrRange(0, size=MAIN_SIZE)
mmio_range = AddrRange(MMIO_BASE, size=MMIO_SIZE)


# ---------- I-side (through caches) ----------
system.cpu.icache_port = system.icache.cpu_side

# ---------- D-side: splitter + two Bridges ----------
system.dsplit = SystemXBar(width=64)
system.cpu.dcache_port = system.dsplit.cpu_side_ports

system.to_l1d = Bridge(delay="1ns", ranges=[main_range])
system.bypass_mmio = Bridge(delay="1ns", ranges=[mmio_range])

system.dsplit.mem_side_ports = [
    system.to_l1d.cpu_side_port,
    system.bypass_mmio.cpu_side_port,
]

# Normal path: Bridge → L1D
system.to_l1d.mem_side_port = system.dcache.cpu_side

# Bypass path: Bridge → membus (skip caches)
system.bypass_mmio.mem_side_port = system.membus.cpu_side_ports

# ---------- L2 <-> membus ----------
system.l2bus.cpu_side_ports = [system.icache.mem_side, system.dcache.mem_side]
system.l2cache.cpu_side = system.l2bus.mem_side_ports
system.l2cache.mem_side = system.membus.cpu_side_ports

# ---------- Walkers ----------
if hasattr(system.cpu, "itb") and hasattr(system.cpu.itb, "walk_port"):
    system.cpu.itb.walk_port = system.membus.cpu_side_ports
if hasattr(system.cpu, "dtb") and hasattr(system.cpu.dtb, "walk_port"):
    system.cpu.dtb.walk_port = system.membus.cpu_side_ports

# ---------- x86 interrupts ----------
system.cpu.createInterruptController()
system.cpu.interrupts[0].pio = system.membus.mem_side_ports
system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports

# system port
system.system_port = system.membus.cpu_side_ports

# --------- dual Ramulator2 controllers ---------
system.ram_main = Ramulator2(
    config_path="ext/ramulator2/ramulator2/gem5_example.yaml",
    output_dir="ramulator_main_out",
)
system.ram_main.range = main_range

system.ram_mmio = Ramulator2(
    config_path="ext/ramulator2/ramulator2/gem5_example.yaml",
    output_dir="ramulator_mmio_out",
)
system.ram_mmio.range = mmio_range

# Both memories connect to membus mem_side
system.membus.mem_side_ports = [system.ram_main.port, system.ram_mmio.port]

# publish memory ranges
system.mem_ranges = [main_range, mmio_range]

# --------- SE workload ---------
binary = "src/learning_gem5/experiments/search_pum/a.out"
system.workload = SEWorkload.init_compatible(binary)  # OS-like SE workload

process = Process()
process.cmd = [binary]
system.cpu.workload = [process]  # <-- make it a list (VectorParam)
system.cpu.createThreads()

# --------- instantiate first, then map ---------
root = Root(full_system=False, system=system)

import m5

m5.instantiate()

# Now the Process C++ object & page table exist; do VA==PA map for MMIO
mmio_size_bytes = toMemorySize(MMIO_SIZE)
process.map(MMIO_BASE, MMIO_BASE, mmio_size_bytes, cacheable=False)

print("Beginning simulation!")
event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {event.getCause()}")
