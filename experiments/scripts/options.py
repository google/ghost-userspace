# Copyright 2021 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Options dataclasses for RocksDB, the Antagonist, and ghOSt.

This file contains the options for RocksDB, the Antagonist, and ghOSt. It also
has functions to convert the options to command line arguments used to start the
applications.
"""

import enum
import functools
import os
from typing import Dict
from typing import List
from dataclasses import dataclass
from dataclasses import field
from dataclasses import fields

_NUM_ROCKSDB_WORKERS = 6
_FIRST_CPU = 10
# The first two CPUs are used for the load generator and either the dispatcher
# (for CFS) or the global agent (for ghOSt).
_FIRST_ROCKSDB_WORKER_CPU = _FIRST_CPU + 2
# The first CPU is used for the load generator.
_FIRST_ANTAGONIST_WORKER_CPU = _FIRST_CPU + 1
# The path to a tmpfs backed by hugepages. This is where the RocksDB,
# Antagonist, and ghOSt binaries are copied to and run from.
TMPFS_MOUNT = "/dev/shm"


@enum.unique
class Scheduler(str, enum.Enum):
  """The experiment scheduler.

  CFS is the Linux Completely Fair Scheduler.
  GHOST is the ghOSt Scheduler.
  """
  CFS = "cfs"
  GHOST = "ghost"


def CheckSchedulers(schedulers: List[str]) -> bool:
  """Checks that `schedulers` contains valid schedulers.

  Args:
    schedulers: The schedulers to check.

  Returns:
    True if `schedulers` contains valid schedulers. False otherwise.
  """
  for scheduler in schedulers:
    try:
      Scheduler(scheduler)
    except ValueError:
      return False
  return True


@enum.unique
class PrintFormat(str, enum.Enum):
  """The print format for results.

  PRETTY is a human-readable print format. It probably does not make sense to
  use this for an automated testing script.
  CSV is a comma-separated value format. This is what should generally be used.
  """
  PRETTY = "pretty"
  CSV = "csv"


@enum.unique
class CfsWaitType(str, enum.Enum):
  """The way that CFS workers wait until they are assigned more work.

  SPIN has the workers spin.
  FUTEX has the workers sleep on a futex.
  """
  SPIN = "spin"
  FUTEX = "futex"


@dataclass
class Paths:
  """The paths to each of the binaries.

  Attributes:
    rocksdb: The path to the RocksDB binary.
    antagonist: The path to the Antagonist binary.
    ghost: The path to the ghOSt binary.
  """
  rocksdb: str = os.path.join(TMPFS_MOUNT, "rocksdb")
  antagonist: str = os.path.join(TMPFS_MOUNT, "antagonist")
  ghost: str = os.path.join(TMPFS_MOUNT, "agent_shinjuku")


def GetDefaultRocksDBWorkerCpus():
  """Returns the default list of worker CPUs for RocksDB.

  This list contains all CPUs in [`_FIRST_ROCKSDB_WORKER_CPU`,
  `_FIRST_ROCKSDB_WORKER_CPU` + `_NUM_ROCKSDB_WORKERS`).

  Returns:
    The default list of worker CPUs for RocksDB.
  """
  return list(
      range(_FIRST_ROCKSDB_WORKER_CPU,
            _FIRST_ROCKSDB_WORKER_CPU + _NUM_ROCKSDB_WORKERS))


def GetDefaultAntagonistWorkerCpus():
  """Returns the default list of worker CPUs for the Antagonist.

  This list contains all CPUs in [`_FIRST_ANTAGONIST_WORKER_CPU`,
  `_FIRST_ANTAGONIST_WORKER_CPU` + `_NUM_ROCKSDB_WORKERS` + 1).

  We add 1 since the Antagonist co-locates one of its threads with the
  dispatcher (for CFS) or the global agent (for ghOSt).

  Returns:
    The default list of worker CPUs for RocksDB.
  """
  return list(
      range(_FIRST_ANTAGONIST_WORKER_CPU,
            _FIRST_ANTAGONIST_WORKER_CPU + _NUM_ROCKSDB_WORKERS + 1))


@dataclass
class RocksDBOptions:
  """The command line arguments passed to RocksDB.

  Attributes:
    print_format: The format that the results are printed in. We want CSV
      because it is easy to parse and graph.
    print_distribution: If True, every single request's results are printed.
    print_ns: If True, the results are printed in units of nanoseconds. If
      False, the results are printed in units of microseconds.
    print_get: Print an additional section in the results for just Get requests.
    print_range: Print an additional section in the results for just Range
      queries.
    rocksdb_db_path: The path to the RocksDB database. If a database does not
      exist at that path, the database is created.
    throughput: The synthetic throughput used in the experiment.
    range_query_ratio: The share of requests that are Range queries. 1 -
      `range_query_ratio` is the share of requests that are Get requests.
    load_generator_cpu: The CPU that the load generator runs on.
    cfs_dispatcher_cpu: For CFS (Linux Completely Fair Scheduler) experiments,
      the CPU that the dispatcher runs on.
    num_workers: The number of workers. Each worker has one thread.
    worker_cpus: For CFS (Linux Completely Fair Scheduler) experiments, the CPUs
      that the workers run on. For ghOSt experiments, this list should be empty
      because ghOSt controls which CPUs the workers run on.
    cpus: The CPUs to run the CFS experiment on. For the ghOSt experiment, this
      specifies the CPU to run the load generator thread on.
    cfs_wait_type: The way that workers wait until they are given more work in
      the CFS experiments. SPIN makes the workers spin whereas FUTEX makes the
      workers sleep on a futex.
    get_duration: The service time of Get requests.
    range_duration: The service time of Range queries.
    get_exponential_mean: If nonzero, a sample from the exponential distribution
      with a mean of `get_exponential_mean` is added to the service time of Get
      requests to generate a new service time. i.e., Get request service time =
      `get_duration` + Exp(1 / `get_exponential_mean`).
    batch: The maximum number of packets passed to a worker at once.
    experiment_duration: The experiment duration.
    discard_duration: The requests service from the experiment start to the end
      of the `discard_duration` duration are discarded so that initialization
      costs, such as page faults, do not impact the results.
    scheduler: The scheduler to use. CFS or GHOST.
    ghost_qos: If ghOSt is used, this is the QoS (Quality-of-Service) class
      assigned to RocksDB threads.
  """
  print_format: PrintFormat = PrintFormat.CSV
  print_distribution: bool = False
  print_ns: bool = False
  print_get: bool = True
  print_range: bool = True
  rocksdb_db_path: str = os.path.join(TMPFS_MOUNT, "orch_db")
  throughput: int = 20000
  range_query_ratio: float = 0.0
  load_generator_cpu: int = _FIRST_CPU
  cfs_dispatcher_cpu: int = _FIRST_CPU + 1
  num_workers: int = _NUM_ROCKSDB_WORKERS
  worker_cpus: List[int] = field(default_factory=GetDefaultRocksDBWorkerCpus)
  cfs_wait_type: CfsWaitType = CfsWaitType.SPIN
  get_duration: str = "10us"
  range_duration: str = "5000us"
  get_exponential_mean: str = "0us"
  batch: int = 1
  experiment_duration: str = "15s"
  discard_duration: str = "2s"
  scheduler: Scheduler = Scheduler.CFS
  ghost_qos: int = 2


@dataclass
class AntagonistOptions:
  """The command line arguments passed to the Antagonist.

  Attributes:
    print_format: The format that the results are printed in. We want CSV
      because it is easy to parse and graph.
    work_share: Each thread tries to consume `work_share` share of the cycles on
      a CPU.
    num_threads: The number of threads to use.
    cpus: The CPUs to run the CFS experiment on. For the ghOSt experiment, this
      specifies the CPU to run the load generator thread on.
    experiment_duration: The experiment duration.
    scheduler: The scheduler to use. CFS or GHOST.
    ghost_qos: If ghOSt is used, this is the QoS (Quality-of-Service) class
      assigned to Antagonist threads.
  """
  print_format: PrintFormat = PrintFormat.CSV
  work_share: float = 1.0
  # Add 1 since the Antagonist is also co-located with the dispatcher (for CFS)
  # and the global agent (for ghOSt).
  num_threads: int = _NUM_ROCKSDB_WORKERS + 1
  # Add 1 since the Antagonist should not be co-located with the load generator.
  cpus: List[int] = field(default_factory=GetDefaultAntagonistWorkerCpus)
  experiment_duration: str = "15s"
  scheduler: Scheduler = Scheduler.CFS
  ghost_qos: int = 1


@dataclass
class GhostOptions:
  """The command line arguments passed to ghOSt.

  Attributes:
    firstcpu: The first CPU to start running ghOSt agents on.
    globalcpu: The CPU to run the global agent on. Note that firstcpu <=
      globalcpu < firstcpu + ncpus
    ncpus: The number of CPUs that ghOSt will run agents on (and therefore
      schedule).
    preemption_time_slice: ghOSt threads that run this duration or longer are
      preempted and added to the end of the runqueue.
  """
  # The load generator is on `_FIRST_CPU`.
  firstcpu: int = _FIRST_CPU + 1
  globalcpu: int = _FIRST_CPU + 1
  # Add 1 to account for the global agent.
  ncpus: int = _NUM_ROCKSDB_WORKERS + 1
  # Turn off time-based preemption by setting the preemption time slice to
  # infinity. Some scheduling algorithms do not have time-based preemption, so
  # scheduling algorithms that do have it should explicitly turn this on.
  preemption_time_slice: str = "inf"


def GetBinaryPaths():
  """Returns the paths to each of the binaries."""
  return Paths()


def GetRocksDBOptions(scheduler: Scheduler, num_cpus: int, num_workers: int):
  """Returns RocksDB options with a default config tuned to the scheduler.

  Args:
    scheduler: The experiment scheduler. CFS or GHOST.
    num_cpus: The number of CPUs used in the experiment.
    num_workers: The number of workers.

  Returns:
    The RocksDB options.
  """
  # CFS requires at least 3 CPUs: one for the load generator, one for the
  # dispatcher, and at least one for workers.
  #
  # ghOSt requires at least 2 CPUS: one for the load generator and at least one
  # for workers.
  if not ((scheduler == Scheduler.CFS and num_cpus >= 3) or
          (scheduler == Scheduler.GHOST and num_cpus >= 2)):
    raise ValueError(
        f"Incorrect number of CPUs specified for {scheduler.value}.")
  if num_workers < 1:
    raise ValueError("There must be at least 1 worker.")

  r = RocksDBOptions()
  r.scheduler = scheduler
  r.load_generator_cpu = _FIRST_CPU
  r.num_workers = num_workers
  if scheduler == Scheduler.CFS:
    # For CFS, each thread is pinned to a unique CPU.
    r.cfs_dispatcher_cpu = _FIRST_CPU + 1
    r.worker_cpus = list(range(_FIRST_CPU + 2, _FIRST_CPU + num_cpus))
  else:
    if scheduler != Scheduler.GHOST:
      raise ValueError(f"Unknown scheduler {scheduler}.")
    # Only the load generator is pinned to a CPU and scheduled with CFS for the
    # ghOSt experiments. The CPU constraints for the global agent and the worker
    # threads are determined by the ghOSt process.
    r.worker_cpus = []
  return r


def GetAntagonistOptions(scheduler: Scheduler, num_cpus: int):
  """Returns Antagonist options with a default configuration tuned to the specified scheduler.

  Args:
    scheduler: The experiment scheduler. CFS or GHOST.
    num_cpus: The number of CPUs used in the experiment.

  Returns:
    The Antagonist options.
  """
  if num_cpus <= 0:
    raise ValueError(
        f"The Antagonist needs at least 1 CPU. {num_cpus} CPUs were specified.")

  a = AntagonistOptions()
  a.scheduler = scheduler
  # There is one thread per CPU.
  a.num_threads = num_cpus
  if scheduler == Scheduler.CFS:
    # The load generator runs on `_FIRST_CPU`, so add 1 so that the antagonist
    # is not co-located with the load generator.
    first_antagonist_cpu = _FIRST_CPU + 1
    a.cpus = list(range(first_antagonist_cpu, first_antagonist_cpu + num_cpus))
  else:
    if scheduler != Scheduler.GHOST:
      raise ValueError(f"Unknown scheduler {scheduler}.")
    # As with RocksDB, the CPU constraints for the global agent and Antagonist
    # worker threads are determined by the ghOSt process.
    a.cpus = []
  return a


def GetGhostOptions(num_cpus: int):
  """Returns ghOSt options with a default configuration.

  Args:
    num_cpus: The number of CPUs used in the experiment.

  Returns:
    The ghOSt options.
  """
  if num_cpus <= 1:
    raise ValueError(
        f"ghOSt needs at least 2 CPUs. {num_cpus} CPUs were specified.")

  g = GhostOptions()
  # The load generator, which is scheduled by CFS, is pinned to `_FIRST_CPU`.
  g.firstcpu = _FIRST_CPU + 1
  g.globalcpu = g.firstcpu
  # Subtract 1 from `num_cpus` because one of the CPUs is used by the load
  # generator, which is scheduled by CFS.
  g.ncpus = num_cpus - 1
  return g


def DictToArgs(d: Dict[str, str]):
  """Converts the dictionary `d` to list of arguments for `subprocess.Popen()`.

  For each (k, v) pair in the dictionary, two entries are added to the list:
  "--k" and "v". So {"sport": "hockey", "goals": 7} is converted to: ["--sport",
  "hockey", "--goals", "7"].

  Note that all keys must be strings. All values are converted to a string
  regardless of their type.

  Args:
    d: The dictionary to convert to a list.

  Returns:
    The list of arguments for `subprocess.Popen()`, generated from `d`.
  """
  return functools.reduce(lambda a, key: a + ["--" + key, d[key]], d, [])


def DataClassToArgs(dc):
  """Convert the dataclass members of `dc` to a list of command line arguments.

  These command line arguments are used to run one of the experiment binaries.

  Args:
    dc: The dataclass instance.

  Returns:
    A list of command line agruments.
  """
  d = dict()
  for f in fields(dc):
    field_value = getattr(dc, f.name)
    if f.type is str:
      d[f.name] = field_value
    elif f.type is int:
      d[f.name] = str(field_value)
    elif f.type is float:
      d[f.name] = str(field_value)
    elif f.type is bool:
      if field_value:
        d[f.name] = ""
      else:
        d[f"no{f.name}"] = ""
    elif f.type is List[int]:
      d[f.name] = ",".join(str(i) for i in field_value)
    elif issubclass(f.type, enum.Enum):
      # We can get the string associated with each enum member by accessing
      # `value.value`.
      d[f.name] = getattr(field_value, "value")
      if not isinstance(d[f.name], str):
        raise ValueError(
            f"The enum value for the {f.name} field is a {type(d[f.name])},"
            f" but it should be a string.")
    else:
      # We covered all types in the data class.
      raise ValueError(
          f"Unsupported type {f.type} for field {f.name} in the data class.")
  # All command line arguments should be strings.
  for (key, value) in d.items():
    if not isinstance(key, str):
      raise ValueError(f"The key {key} is not a string.")
    if not isinstance(value, str):
      raise ValueError(f"The value {value} is not a string.")

  return DictToArgs(d)


def CreateCgroup(name: str):
  """Creates a cgroup.

  Creates a cgroup with name `name` to control both CPU and memory resources.
  Uses cgroups v1.

  Args:
    name: The cgroup name.
  """
  os.system(f"mkdir -p /dev/cgroup/cpu/{name}")
  os.system(f"echo 102400 > /dev/cgroup/cpu/{name}/cpu.shares")
  os.system(f"mkdir -p /dev/cgroup/memory/{name}")
  os.system(f"echo 60000M > /dev/cgroup/memory/{name}/memory.limit_in_bytes")


def MoveProcessToCgroup(name: str, pid: int):
  """Moves a process to a cgroup.

  Moves process with PID `pid` to a CPU cgroup and a memory cgroup with name
  `name`. Uses cgroups v1.

  Args:
    name: The cgroup name.
    pid: The process PID.
  """
  os.system(f"echo {pid} > /dev/cgroup/cpu/{name}/tasks")
  os.system(f"echo {pid} > /dev/cgroup/memory/{name}/tasks")


def GetContainerArgs(name: str):
  """Returns the command/arguments to start a process in a container.

  Args:
    name: The container name.

  Returns:
    The list with the command and arguments.
  """
  return [
      "container.py",
      "run",
      "--cpurate",
      "100",
      "--ram",
      "60000",
      "--overwrite",
      name,
      "--",
  ]


def GetNiceArgs(level: int):
  """Returns the command/arguments to set the `nice` level of a new process.

  Args:
    level: The nice level to set (-20 <= `level` <= 19).
  """
  if level < -20 or level > 19:
    raise ValueError(
        f"The level must be >= -20 and <= 19. The level specified is {level}.")
  return ["nice", "-n", str(level)]
