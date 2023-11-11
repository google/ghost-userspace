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
from experiments.scripts.options import GetGhostOptions
from experiments.scripts.options import GetRocksDBOptions
from experiments.scripts.options import Scheduler
from experiments.scripts.run import Experiment
from experiments.scripts.run import Run

_NUM_CPUS = 8
_NUM_GHOST_WORKERS = 200


def RunGhost(
    preemption_interval_us: int,
    min_throughput_ps: int,
    max_throughput_ps: int,
    throughput_ps_step: int,
):
    """Runs the ghOSt experiment."""
    e: Experiment = Experiment()
    e.throughputs = list(
        i
        for i in range(
            min_throughput_ps,
            max_throughput_ps + throughput_ps_step,
            throughput_ps_step,
        )
    )
    e.rocksdb = GetRocksDBOptions(Scheduler.GHOST, _NUM_CPUS, _NUM_GHOST_WORKERS)
    e.rocksdb.range_query_ratio = 0.005
    e.antagonist = None
    e.ghost = GetGhostOptions(_NUM_CPUS)
    e.ghost.preemption_time_slice = str(preemption_interval_us) + "us"

    Run(e)


def main(argv: Sequence[str]):
    if len(argv) != 5:
        raise app.UsageError(
            "Usage: shinjuku_dynamic_interval.par <preemption_interval_us>\n"
            "<min_throughput_ps> <max_throughput_ps> <throughput_ps_step>"
        )

    preemption_interval_us = int(argv[1])
    min_throughput_ps = int(argv[2])
    max_throughput_ps = int(argv[3])
    throughput_ps_step = int(argv[4])

    # Run the experiments.
    RunGhost(
        preemption_interval_us, min_throughput_ps, max_throughput_ps, throughput_ps_step
    )


if __name__ == "__main__":
    app.run(main)
