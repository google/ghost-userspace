# Copyright 2021 Google LLC
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd
"""Runs the RocksDB Shinjuku experiments.

This script runs the RocksDB Shinjuku experiments on ghOSt and on CFS. In these
experiments, there is a centralized FIFO queue maintained for RocksDB requests.
For ghOSt, long requests that exceed their time slice are preempted so that they
do not prevent short requests from running (i.e., ghOSt prevents head-of-line
blocking). The preempted requests are added to the back of the FIFO. For CFS,
requests are run to completion.
"""

from typing import Sequence
from absl import app
from experiments.scripts.options import CheckSchedulers
from experiments.scripts.options import GetGhostOptions
from experiments.scripts.options import GetRocksDBOptions
from experiments.scripts.options import Scheduler
from experiments.scripts.run import Experiment
from experiments.scripts.run import Run

_NUM_CPUS = 8
_NUM_CFS_WORKERS = _NUM_CPUS - 2
_NUM_GHOST_WORKERS = 200


def RunCfs():
  """Runs the CFS (Linux Completely Fair Scheduler) experiment."""
  e: Experiment = Experiment()
  # Run throughputs 10000, 20000, 30000, and 40000.
  e.throughputs = list(i for i in range(10000, 50000, 10000))
  # Toward the end, run throughputs 50000, 51000, 52000, ..., 80000.
  e.throughputs.extend(list(i for i in range(50000, 81000, 1000)))
  e.rocksdb = GetRocksDBOptions(Scheduler.CFS, _NUM_CPUS, _NUM_CFS_WORKERS)
  e.rocksdb.range_query_ratio = 0.005
  e.antagonist = None
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
  e.antagonist = None
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
