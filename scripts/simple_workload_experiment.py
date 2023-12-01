#!/usr/bin/python3

import argparse
from decimal import Decimal
import subprocess


parser = argparse.ArgumentParser()
parser.add_argument(
    "--orca_port", type=int, required=True, help="the port of the running Orca server"
)


def run_experiment(
    orca_port: int,
    sched_type: str,
    throughput: int,
    runtime: int,
    num_workers: int,
    proportion_long_jobs: Decimal,
    preemption_interval_us: int | None = None,
) -> dict[str, str]:
    "Run the experiment and return the CSV portion of the results."

    # Set current ghOSt scheduler
    if sched_type != "cfs":
        cmd = f"scripts/orca_client.sh {orca_port} setsched {sched_type}"
        if preemption_interval_us is not None:
            cmd += f" {preemption_interval_us}"
        subprocess.run(cmd, check=True)

    # Run simple workload
    proc = subprocess.run(
        "scripts/run.sh tests/custom/simple_workload.cc "
        f"{'cfs' if sched_type == 'cfs' else 'g'} "
        f"{throughput} {runtime} {num_workers} {proportion_long_jobs}",
        capture_output=True,
        check=True,
    )

    # Parse CSV portion of output and return it


def main() -> None:
    args = parser.parse_args()
    print(type(args.orca_port))


if __name__ == "__main__":
    main()
