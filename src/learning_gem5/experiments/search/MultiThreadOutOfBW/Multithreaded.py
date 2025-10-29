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


# --------- Classic caches ---------
class L1ICache(Cache):
    def __init__(self, size="512KiB", assoc=8):
        super().__init__()
        self.size = size
        self.assoc = assoc
        self.tag_latency = 2
        self.data_latency = 2
        self.response_latency = 2
        self.mshrs = 32
        self.tgts_per_mshr = 32


class L1DCache(Cache):
    def __init__(self, size="512KiB", assoc=8):
        super().__init__()
        self.size = size
        self.assoc = assoc
        self.tag_latency = 2
        self.data_latency = 2
        self.response_latency = 2
        self.mshrs = 64
        self.tgts_per_mshr = 64


class L2Cache(Cache):
    def __init__(self, size="12MiB", assoc=12):
        super().__init__()
        self.size = size
        self.assoc = assoc
        self.tag_latency = 12
        self.data_latency = 12
        self.response_latency = 12
        self.mshrs = 128
        self.tgts_per_mshr = 64


# --------- build system ---------
system = System()
system.mem_mode = "timing"
system.clk_domain = SrcClockDomain(
    clock="3GHz", voltage_domain=VoltageDomain()
)

# buses
system.membus = SystemXBar(width=64)
system.l2bus = L2XBar()

# O3 CPUs with n cores
NCORES = 33               # number of cores
THREADS_PER_CORE = 1     # SMT per core (DerivO3 supports SMT)
system.cpu = [DerivO3CPU() for _ in range(NCORES)]
for cpu in system.cpu:
    cpu.numThreads = THREADS_PER_CORE
    cpu.branchPred = LocalBP()


# caches
system.icaches = [L1ICache() for _ in range(NCORES)]
system.dcaches = [L1DCache() for _ in range(NCORES)]
system.l2cache = L2Cache()

# ---------- Address map  ----------
MAIN_SIZE = "8GiB"
MMIO_BASE = 0x2_0000_0000  # 8 GiB boundary
MMIO_SIZE = "16GiB"  # size of the MMIO window

main_range = AddrRange(0, size=MAIN_SIZE)
mmio_range = AddrRange(MMIO_BASE, size=MMIO_SIZE)

# ---------- I-side (through caches) ----------
for i in range(NCORES):
    system.cpu[i].icache_port = system.icaches[i].cpu_side

# ---------- D-side: splitter + two Bridges ----------
system.dsplit = [SystemXBar(width=64) for _ in range(NCORES)]
for i in range(NCORES):
    system.cpu[i].dcache_port = system.dsplit[i].cpu_side_ports

system.to_l1d = [
    Bridge(delay="1ns", ranges=[main_range]) for _ in range(NCORES)
]
MMIO_FWD_NS = 6

system.bypass_mmio = [
    Bridge(delay=f"{MMIO_FWD_NS}ns", ranges=[mmio_range])  # <- only hits MMIO addrs
    for _ in range(NCORES)
]

for i in range(NCORES):
    system.dsplit[i].mem_side_ports = [
        system.to_l1d[i].cpu_side_port,
        system.bypass_mmio[i].cpu_side_port,
    ]

# Normal path: Bridge → L1D
for i in range(NCORES):
    system.to_l1d[i].mem_side_port = system.dcaches[i].cpu_side

# Bypass path: Bridge → membus (skip caches)
for i in range(NCORES):
    system.bypass_mmio[i].mem_side_port = system.membus.cpu_side_ports

# ---------- L2 <-> membus ----------
system.l2bus.cpu_side_ports = [
    system.icaches[i].mem_side for i in range(NCORES)
] + [system.dcaches[i].mem_side for i in range(NCORES)]
system.l2cache.cpu_side = system.l2bus.mem_side_ports
system.l2cache.mem_side = system.membus.cpu_side_ports

# ---------- Walkers ----------
for i in range(NCORES):
    if hasattr(system.cpu[i], "itb") and hasattr(
        system.cpu[i].itb, "walk_port"
    ):
        system.cpu[i].itb.walk_port = system.membus.cpu_side_ports
    if hasattr(system.cpu[i], "dtb") and hasattr(
        system.cpu[i].dtb, "walk_port"
    ):
        system.cpu[i].dtb.walk_port = system.membus.cpu_side_ports

# ---------- x86 interrupts ----------
for i in range(NCORES):
    system.cpu[i].createInterruptController()
    system.cpu[i].interrupts[0].pio = system.membus.mem_side_ports
    system.cpu[i].interrupts[0].int_requestor = system.membus.cpu_side_ports
    system.cpu[i].interrupts[0].int_responder = system.membus.mem_side_ports

# system port
system.system_port = system.membus.cpu_side_ports

# --------- Main memory controller ---------
system.ram_main = Ramulator2(
    config_path="ext/ramulator2/ramulator2/gem5_base_ram.yaml",
    output_dir="ramulator_main_out",
)
system.ram_main.range = main_range

# --------- MMIO window split across N channels ---------
CHANNELS_MMIO = (
    2
)

mmio_total_bytes = toMemorySize(MMIO_SIZE)
slice_bytes = mmio_total_bytes // CHANNELS_MMIO

mmio_slices = []
mmio_chans = []
for ch in range(CHANNELS_MMIO):
    start = MMIO_BASE + ch * slice_bytes
    stripe = AddrRange(start, size=slice_bytes)
    mmio_slices.append(stripe)

    ram = Ramulator2(
        config_path="ext/ramulator2/ramulator2/gem5_pum_ram.yaml",
        output_dir=f"ramulator_mmio_ch{ch}_out",
    )
    ram.range = stripe
    ram.null = False
    mmio_chans.append(ram)

for i, ram in enumerate(mmio_chans):
    setattr(system, f"ram_mmio_ch{i}", ram)

# Connect all memory controllers to membus (main + all mmio channels)
system.membus.mem_side_ports = [system.ram_main.port] + [
    r.port for r in mmio_chans
]
# publish memory ranges (logical view). Keeping the contiguous MMIO window is fine.
system.mem_ranges = [main_range, mmio_range]

# --------- SE workload ---------
binary = "src/learning_gem5/experiments/search/MultiThreadOutOfBW/a.out"
system.workload = SEWorkload.init_compatible(binary)

process = Process()
process.cmd = [binary]

# If you want OpenMP/pthreads to use all logical contexts:
total_logical = NCORES * THREADS_PER_CORE
process.env = [
    f"OMP_NUM_THREADS={total_logical}",
    "OMP_PROC_BIND=spread",
    "OMP_PLACES=cores",
]

# IMPORTANT: give each CPU a list of length = THREADS_PER_CORE
for cpu in system.cpu:
    cpu.workload = [process] * THREADS_PER_CORE  # same proc replicated
    cpu.createThreads()
# --------- instantiate first, then map ---------
root = Root(full_system=False, system=system)

import m5

m5.instantiate()

# VA==PA map for the MMIO window (non-cacheable)
mmio_size_bytes = toMemorySize(MMIO_SIZE)
process.map(MMIO_BASE, MMIO_BASE, mmio_size_bytes, cacheable=False)


print("Beginning simulation!")
event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {event.getCause()}")
