# Copyright 2021 Google LLC
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd
"""Runs the RocksDB centralized-queuing experiments.

This script runs the centralized-queuing RocksDB experiments on ghOSt and on
CFS. In these experiments, there is a centralized queue maintained for RocksDB
requests and the requests are not reordered or preempted. This script should be
run on a machine with an Intel Xeon Platinum 8173M as that is what we used in
the paper. If another CPU is used, the throughput ranges below should be
adjusted.
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
_NUM_GHOST_WORKERS = 11


def RunCfs():
  """Runs the CFS (Linux Completely Fair Scheduler) experiment."""
  e: Experiment = Experiment()
  # Run throughputs 10000, 20000, 30000, ... 440000.
  e.throughputs = list(i for i in range(10000, 441000, 10000))
  # Toward the end, run throughputs 450000, 451000, 452000, ..., 480000.
  e.throughputs.extend(list(i for i in range(450000, 481000, 1000)))
  e.rocksdb = GetRocksDBOptions(Scheduler.CFS, _NUM_CPUS, _NUM_CFS_WORKERS)
  e.rocksdb.get_exponential_mean = '1us'
  e.antagonist = None
  e.ghost = None

  Run(e)


def RunGhost(ratio: float = 0.005, tput_start: int = 10000, tput_end:int = 151000, tput_step:int = 10000):
  """Runs the ghOSt experiment."""
  e: Experiment = Experiment()
  # Run throughputs 10000, 20000, 30000, ..., 420000.
  # e.throughputs = list(i for i in range(10000, 421000, 10000))
  # Toward the end, run throughputs 430000, 431000, 432000, ..., 460000.
  e.throughputs = list(i for i in range(tput_start, tput_end, tput_step))
  # e.throughputs.extend(list(i for i in range(430000, 461000, 1000)))
  e.rocksdb = GetRocksDBOptions(Scheduler.GHOST, _NUM_CPUS, _NUM_GHOST_WORKERS)
  # e.rocksdb.get_exponential_mean = '1us'
  e.rocksdb.range_query_ratio = ratio
  e.antagonist = None
  e.ghost = GetGhostOptions(_NUM_CPUS)
  # There is no time-based preemption for centralized queuing, so set the
  # preemption time slice to infinity.
  e.ghost.preemption_time_slice = 'inf'

  Run(e)


def main(argv: Sequence[str]):
  # if len(argv) > 3:
  #   raise app.UsageError('Too many command-line arguments.')
  # elif len(argv) == 1:
  #   raise app.UsageError(
  #       'No experiment specified. Pass `cfs` and/or `ghost` as arguments.')

  # First check that all of the command line arguments are valid.
  if not CheckSchedulers([argv[1]]):
    raise ValueError('Invalid scheduler specified.')

  ratio = float(argv[2])
  tput_start = int(argv[3])
  tput_end = int(argv[4])
  tput_step = int(argv[5])

  scheduler = Scheduler(argv[1])


  # Run the experiments.
  # for i in range(1, len(argv)):
  #   scheduler = Scheduler(argv[i])
  if scheduler == Scheduler.CFS:
    RunCfs()
  else:
    if scheduler != Scheduler.GHOST:
      raise ValueError(f'Unknown scheduler {scheduler}.')
    RunGhost(ratio=ratio,tput_start=tput_start, tput_end=tput_end, tput_step=tput_step )


if __name__ == '__main__':
  app.run(main)
