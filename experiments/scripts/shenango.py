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

This script runs the RocksDB Shenango experiments on ghOSt and on CFS. In these
experiments, RocksDB is co-located with an Antagonist. Specifically, the
dispatcher and worker threads are co-located with the Antagonist threads while
the load generator is isolated on its own CPU (to ensure that the load we think
we are generating is the load we are actually generating). For ghOSt, the
Antagonist threads are preempted to allow RocksDB threads to run. For CFS, this
preemption is left to CFS to figure out. Furthermore, for the CFS experiments,
the worker threads sleep on a futex when they do not have work rather than spin
so that CFS gives the Antagonist threads a chance to run.
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
_NUM_GHOST_WORKERS = 11
# Subtract 1 for the Antagonist since the Antagonist does not run a thread on
# the same CPU as the load generator.
_NUM_ANTAGONIST_CPUS = _NUM_CPUS - 1


def RunCfs():
  """Runs the CFS (Linux Completely Fair Scheduler) experiment."""
  e: Experiment = Experiment()
  # Run throughputs 10000, 20000, 30000, ... 60000.
  e.throughputs = list(i for i in range(10000, 600000, 10000))
  # Toward the end, run throughputs 70000, 71000, 72000, ..., 120000.
  e.throughputs.extend(list(i for i in range(70000, 121000, 1000)))
  e.rocksdb = GetRocksDBOptions(Scheduler.CFS, _NUM_CPUS, _NUM_CFS_WORKERS)
  e.rocksdb.cfs_wait_type = CfsWaitType.FUTEX
  e.rocksdb.get_exponential_mean = '1us'
  e.antagonist = GetAntagonistOptions(Scheduler.CFS, _NUM_ANTAGONIST_CPUS)
  e.ghost = None

  Run(e)


def RunGhost():
  """Runs the ghOSt experiment."""
  e: Experiment = Experiment()
  # Run throughputs 10000, 20000, 30000, ..., 380000.
  e.throughputs = list(i for i in range(10000, 381000, 10000))
  # Toward the end, run throughputs 390000, 391000, 392000, ..., 450000.
  e.throughputs.extend(list(i for i in range(390000, 451000, 1000)))
  e.rocksdb = GetRocksDBOptions(Scheduler.GHOST, _NUM_CPUS, _NUM_GHOST_WORKERS)
  e.rocksdb.get_exponential_mean = '1us'
  e.rocksdb.ghost_qos = 2
  e.antagonist = GetAntagonistOptions(Scheduler.GHOST, _NUM_ANTAGONIST_CPUS)
  e.antagonist.ghost_qos = 1
  e.ghost = GetGhostOptions(_NUM_CPUS)
  # There is no time-based preemption for Shenango, so set the preemption time
  # slice to infinity.
  e.ghost.preemption_time_slice = 'inf'

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
