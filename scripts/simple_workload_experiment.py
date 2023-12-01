#!/usr/bin/python3

import argparse
from decimal import Decimal
import subprocess
from typing import Dict, Optional


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
    preemption_interval_us: Optional[int] = None,
) -> Dict[str, str]:
    "Run the experiment and return the CSV portion of the results."

    # Set current ghOSt scheduler
    if sched_type != "cfs":
        cmdargs = ["scripts/orca_client.sh", str(orca_port), "setsched", sched_type]
        if preemption_interval_us is not None:
            cmdargs.append(str(preemption_interval_us))
        subprocess.run(cmdargs, check=True)

    # Run simple workload
    proc = subprocess.run(
        [
            "scripts/run.sh",
            "tests/custom/simple_workload.cc",
            "cfs" if sched_type == "cfs" else "g",
            str(throughput),
            str(runtime),
            str(num_workers),
            str(proportion_long_jobs),
        ],
        capture_output=True,
        check=True,
    )

    # Parse CSV portion of output and return it
    lines = [line.decode("utf-8").strip() for line in proc.stdout.splitlines()]
    csvlines = lines[(lines.index("<csv>") + 1) : lines.index("</csv>")]
    assert len(csvlines) == 2
    keys = [key for key in csvlines[0].split(", ")]
    values = [value for value in csvlines[1].split(", ")]
    print(keys, values)
    return dict(zip(keys, values))


def main() -> None:
    args = parser.parse_args()
    print(
        run_experiment(
            orca_port=args.orca_port,
            sched_type="dFCFS",
            throughput=10000,
            runtime=5,
            num_workers=10,
            proportion_long_jobs=Decimal("0.01"),
        )
    )


if __name__ == "__main__":
    main()
