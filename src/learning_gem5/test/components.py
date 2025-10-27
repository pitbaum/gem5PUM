from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.cachehierarchies.classic.private_l1_shared_l2_cache_hierarchy import (
    PrivateL1SharedL2CacheHierarchy,
)
from gem5.components.memory.ramulator_2 import Ramulator2System
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.simulator import Simulator

cache_hierarchy = PrivateL1SharedL2CacheHierarchy(
    l1d_size="64KiB", l1i_size="64KiB", l2_size="8MiB"
)
# cache_hierarchy = NoCache()

# memory = SingleChannelDDR4_2400()
memory = Ramulator2System(
    "ext/ramulator2/ramulator2/gem5_example.yaml", "ramulator_out", "8GB"
)


processor = SimpleProcessor(cpu_type=CPUTypes.TIMING, num_cores=1, isa=ISA.X86)

board = SimpleBoard(
    clk_freq="3GHz",
    cache_hierarchy=cache_hierarchy,
    memory=memory,
    processor=processor,
)

binary = BinaryResource(local_path="src/learning_gem5/test/pattern")
board.set_se_binary_workload(binary)

# Lastly we instantiate the simulator module and simulate the program.
print("set simulator")
simulator = Simulator(board=board)
simulator.run()

# We acknowlwdge the user that the simulation has ended.
print(
    "Exiting @ tick {} because {}.".format(
        simulator.get_current_tick(),
        simulator.get_last_exit_event_cause(),
    )
)
