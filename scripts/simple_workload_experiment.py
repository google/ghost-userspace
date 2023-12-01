#!/usr/bin/python3

import argparse
import csv
from decimal import Decimal
import subprocess
from typing import Any, List, Tuple


parser = argparse.ArgumentParser()
parser.add_argument(
    "--orca_port", type=int, required=True, help="the port of the running Orca server"
)
parser.add_argument(
    "--out_file", type=str, required=True, help="output file for experiment results"
)


def run_experiment(
    orca_port: int,
    sched_type: str,
    throughput: int,
    runtime: int,
    num_workers: int,
    proportion_long_jobs: Decimal,
    preemption_interval_us: int = 0,
) -> List[Tuple[str, str]]:
    "Run the experiment and return the CSV portion of the results."

    # Set current ghOSt scheduler
    if sched_type != "cfs":
        cmdargs = ["scripts/orca_client.sh", str(orca_port), "setsched", sched_type]
        if preemption_interval_us > 0:
            cmdargs.append(str(preemption_interval_us))
        subprocess.run(cmdargs, check=True)

    # Run simple workload
    print("Running simple_workload")
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
        stdout=subprocess.PIPE,
        check=True,
    )

    # Parse CSV portion of output and return it
    lines = [line.decode("utf-8").strip() for line in proc.stdout.splitlines()]
    csvlines = lines[(lines.index("<csv>") + 1) : lines.index("</csv>")]
    assert len(csvlines) == 2
    keys = [key for key in csvlines[0].split(", ")]
    values = [value for value in csvlines[1].split(", ")]
    return list(zip(keys, values))


def main() -> None:
    args = parser.parse_args()
    orca_port: int = args.orca_port
    out_file: str = args.out_file

    csvrows: List[List[Any]] = []

    for sched_type in ["dFCFS", "cFCFS", "cfs"]:
        for throughput in range(5000, 20000 + 1, 5000):
            for proportion_long_jobs in [Decimal("0.01"), Decimal("0.5")]:
                for trial in range(5):
                    preemption_interval_us = 500 if sched_type == "cFCFS" else 0
                    stats = run_experiment(
                        orca_port=orca_port,
                        sched_type=sched_type,
                        throughput=throughput,
                        runtime=5,
                        num_workers=10,
                        proportion_long_jobs=proportion_long_jobs,
                        preemption_interval_us=preemption_interval_us,
                    )
                    if len(csvrows) == 0:
                        csvrows.append(
                            [
                                "trial",
                                "sched_type",
                                "preemption_interval_us",
                                "throughput",
                                "proportion_long_jobs",
                            ]
                            + [t[0] for t in stats]
                        )
                    print(
                        f"Finished experiment for sched_type={sched_type} throughput={throughput} proportion_long_jobs={proportion_long_jobs} trial={trial}"
                    )
                    csvrows.append(
                        [
                            trial,
                            sched_type,
                            preemption_interval_us,
                            throughput,
                            proportion_long_jobs,
                        ]
                        + [t[1] for t in stats]
                    )

    with open(out_file, mode="w") as file:
        writer = csv.writer(file)

        for row in csvrows:
            writer.writerow(row)

    print("Finished!")


if __name__ == "__main__":
    main()
