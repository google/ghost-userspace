# Copyright 2021 Google LLC
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd
"""Runs the synthetic workload experiments.

This script runs the synthetic workload experiments on ghOSt and on CFS. In these
experiments, there is a centralized FIFO queue maintained for requests.
For ghOSt, long requests that exceed their time slice are preempted so that they
do not prevent short requests from running (i.e., ghOSt prevents head-of-line
blocking). The preempted requests are added to the back of the FIFO. For CFS,
requests are run to completion.
"""

import os
from typing import Sequence
from absl import app
from experiments.scripts.options import TMPFS_MOUNT, CheckSchedulers, GetBinaryPaths
from experiments.scripts.options import GetGhostOptions
from experiments.scripts.options import GetRocksDBOptions
from experiments.scripts.options import Scheduler
from experiments.scripts.run import Experiment
from experiments.scripts.run import Run

_NUM_CPUS = 8
_NUM_CFS_WORKERS = _NUM_CPUS - 2
_NUM_GHOST_WORKERS = 200


def RunCfs(
    ratio: float = 0.005,
    tput_start: int = 10000,
    tput_end: int = 151000,
    tput_step: int = 10000,
    exp_duration: str = "30s",
):
    """Runs the CFS (Linux Completely Fair Scheduler) experiment."""
    e: Experiment = Experiment()
    # Run throughputs 10000, 20000, 30000, and 40000.
    # e.throughputs = list(i for i in range(10000, 50000, 10000))
    # Toward the end, run throughputs 50000, 51000, 52000, ..., 80000.
    e.throughputs = list(i for i in range(tput_start, tput_end, tput_step))
    # e.throughputs.extend(list(i for i in range(50000, 81000, 1000)))
    e.rocksdb = GetRocksDBOptions(Scheduler.CFS, _NUM_CPUS, _NUM_CFS_WORKERS)
    e.rocksdb.experiment_duration = exp_duration
    e.rocksdb.range_query_ratio = ratio
    e.antagonist = None
    e.ghost = None

    Run(e)


def RunGhost(
    ratio: float = 0.005,
    time_slice: str = "30us",
    tput_start: int = 10000,
    tput_end: int = 151000,
    tput_step: int = 10000,
    agent: str = "agent_shinjuku",
    exp_duration: str = "30s",
):
    """Runs the ghOSt experiment."""
    e: Experiment = Experiment()
    # Run throughputs 1000, 20000, 30000, ..., 130000.
    # e.throughputs = list(i for i in range(10000, 140000, 10000))
    e.throughputs = list(i for i in range(tput_start, tput_end, tput_step))
    # Toward the end, run throughputs 140000, 141000, 142000, ..., 150000.
    # e.throughputs.extend(list(i for i in range(140000, 151000, 1000)))
    e.rocksdb = GetRocksDBOptions(Scheduler.GHOST, _NUM_CPUS, _NUM_GHOST_WORKERS)
    e.rocksdb.range_query_ratio = ratio
    e.rocksdb.experiment_duration = exp_duration
    e.antagonist = None
    e.binaries = GetBinaryPaths()
    e.binaries.ghost = os.path.join(TMPFS_MOUNT, agent)
    print("Bin ghost path:", e.binaries.ghost)
    e.ghost = GetGhostOptions(_NUM_CPUS)
    if agent == "agent_shinjuku":
        e.ghost.preemption_time_slice = time_slice
    else:
        e.ghost.preemption_time_slice = "inf"

    Run(e)


def main(argv: Sequence[str]):
    # if len(argv) > 5:
    #   raise app.UsageError('Too many command-line arguments.')
    # elif len(argv) == 1:
    #   raise app.UsageError(
    #       'No experiment specified. Pass `cfs` and/or `ghost` as arguments.')

    if len(argv) != 9:
        raise app.UsageError(
            "Usage: synthetic.py scheduler ratio time_slice tput_start tput_end tput_step agent exp_duration\n"
            "Example: synthetic.py ghost 0.01 30us 10000 300000 10000 agent_shinjuku 30s"
        )

    if argv[1][0] == "c":
        scheduler = Scheduler.CFS
    elif argv[1][0] == "g":
        scheduler = Scheduler.GHOST
    else:
        raise ValueError(f"Unknown scheduler {argv[1]}.")

    ratio = float(argv[2])
    time_slice = str(argv[3])
    tput_start = int(argv[4])
    tput_end = int(argv[5])
    tput_step = int(argv[6])
    agent = str(argv[7])
    exp_duration = str(argv[8])

    if scheduler == Scheduler.CFS:
        RunCfs(
            ratio,
            tput_start=tput_start,
            tput_end=tput_end,
            tput_step=tput_step,
            exp_duration=exp_duration,
        )
    else:
        print("Agent", agent, "exp_duration", exp_duration)
        RunGhost(
            ratio,
            time_slice,
            tput_start=tput_start,
            tput_end=tput_end,
            tput_step=tput_step,
            exp_duration=exp_duration,
            agent=agent,
        )


if __name__ == "__main__":
    app.run(main)
