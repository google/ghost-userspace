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
"""Runs the RocksDB Shenango experiments.

This script runs the RocksDB Shinjuku+Shenango experiments on ghOSt and CFS. The
experiments contain a mix of short and long requests (Shinjuku) and the RocksDB
threads are co-located with Antagonist threads (Shenango).
"""

from typing import Sequence
from absl import app
from experiments.scripts.options import CfsWaitType
from experiments.scripts.options import CheckSchedulers
from experiments.scripts.options import GetAntagonistOptions
from experiments.scripts.options import GetGhostOptions
from experiments.scripts.options import GetRocksDBOptions
from experiments.scripts.options import Scheduler
from experiments.scripts.run import Experiment
from experiments.scripts.run import Run

_NUM_CPUS = 8
_NUM_CFS_WORKERS = _NUM_CPUS - 2
_NUM_GHOST_WORKERS = 200
# Subtract 1 for the Antagonist since the Antagonist does not run a thread on
# the same CPU as the load generator.
_NUM_ANTAGONIST_CPUS = _NUM_CPUS - 1


def RunCfs():
  """Runs the CFS (Linux Completely Fair Scheduler) experiment."""
  e: Experiment = Experiment()
  # Run throughputs 10000, 20000, 30000, and 40000.
  e.throughputs = list(i for i in range(10000, 50000, 10000))
  # Toward the end, run throughputs 50000, 51000, 52000, ..., 80000.
  e.throughputs.extend(list(i for i in range(50000, 81000, 1000)))
  e.rocksdb = GetRocksDBOptions(Scheduler.CFS, _NUM_CPUS, _NUM_CFS_WORKERS)
  e.rocksdb.range_query_ratio = 0.005
  e.rocksdb.cfs_wait_type = CfsWaitType.FUTEX
  e.antagonist = GetAntagonistOptions(Scheduler.CFS, _NUM_ANTAGONIST_CPUS)
  e.ghost = None

  Run(e)


def RunGhost():
  """Runs the ghOSt experiment."""
  e: Experiment = Experiment()
  # Run throughputs 1000, 20000, 30000, ..., 130000.
  e.throughputs = list(i for i in range(10000, 140000, 10000))
  # Toward the end, run throughputs 140000, 141000, 142000, ..., 150000.
  e.throughputs.extend(list(i for i in range(140000, 151000, 1000)))
  e.rocksdb = GetRocksDBOptions(Scheduler.GHOST, _NUM_CPUS, _NUM_GHOST_WORKERS)
  e.rocksdb.range_query_ratio = 0.005
  e.rocksdb.ghost_qos = 2
  e.antagonist = GetAntagonistOptions(Scheduler.GHOST, _NUM_ANTAGONIST_CPUS)
  e.antagonist.ghost_qos = 1
  e.ghost = GetGhostOptions(_NUM_CPUS)
  e.ghost.preemption_time_slice = '30us'

  Run(e)


def main(argv: Sequence[str]):
  if len(argv) > 3:
    raise app.UsageError('Too many command-line arguments.')
  elif len(argv) == 1:
    raise app.UsageError(
        'No experiment specified. Pass `cfs` and/or `ghost` as arguments.')

  # First check that all of the command line arguments are valid.
  if not CheckSchedulers(argv[1:]):
    raise ValueError('Invalid scheduler specified.')

  # Run the experiments.
  for i in range(1, len(argv)):
    scheduler = Scheduler(argv[i])
    if scheduler == Scheduler.CFS:
      RunCfs()
    else:
      if scheduler != Scheduler.GHOST:
        raise ValueError(f'Unknown scheduler {scheduler}.')
      RunGhost()


if __name__ == '__main__':
  app.run(main)
