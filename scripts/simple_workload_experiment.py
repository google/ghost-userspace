#!/usr/bin/python3

import argparse
import csv
from decimal import Decimal
import subprocess
from typing import List, Tuple


parser = argparse.ArgumentParser()
parser.add_argument(
    "--orca_port", type=int, required=True, help="the port of the running Orca server"
)
parser.add_argument(
    "--out_file", type=str, required=True, help="output file for experiment results"
)


def set_scheduler(
    orca_port: int, sched_type: str, preemption_interval_us: int = 0
) -> None:
    "Set the scheduler via orca_client."

    if sched_type != "cfs":
        cmdargs = ["scripts/orca_client.sh", str(orca_port), "setsched", sched_type]
        if preemption_interval_us > 0:
            cmdargs.append(str(preemption_interval_us))
        subprocess.run(cmdargs, check=True)


def run_experiment(
    sched_type: str,
    throughput: int,
    runtime: int,
    num_workers: int,
    proportion_long_jobs: Decimal,
) -> List[Tuple[str, str]]:
    "Run the experiment and return the CSV portion of the results."

    # Run simple workload
    print("Running simple_workload")
    proc = subprocess.run(
        [
            "bazel-bin/simple_workload",
            "cfs" if sched_type == "cfs" else "ghost",
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


def run_sched_exp(orca_port: int, sched_type: str) -> List[List[str]]:
    "Run the experiment for one scheduler type."

    rows: List[List[str]] = []

    preemption_interval_us = 500 if sched_type == "cFCFS" else 0
    set_scheduler(
        orca_port=orca_port,
        sched_type=sched_type,
        preemption_interval_us=preemption_interval_us,
    )

    for throughput in range(20000, 200000 + 1, 20000):
        for proportion_long_jobs in [
            Decimal("0"),
            Decimal("0.01"),
            Decimal("0.1"),
            Decimal("0.5"),
        ]:
            print(
                f"Starting experiment for sched_type={sched_type} "
                f"throughput={throughput} "
                f"proportion_long_jobs={proportion_long_jobs}"
            )
            stats = run_experiment(
                sched_type=sched_type,
                throughput=throughput,
                runtime=5,
                num_workers=10,
                proportion_long_jobs=proportion_long_jobs,
            )
            if len(rows) == 0:
                rows.append(
                    [
                        "sched_type",
                        "preemption_interval_us",
                        "throughput",
                        "proportion_long_jobs",
                    ]
                    + [t[0] for t in stats]
                )
            rows.append(
                [
                    sched_type,
                    str(preemption_interval_us),
                    str(throughput),
                    str(proportion_long_jobs),
                ]
                + [t[1] for t in stats]
            )

    return rows


def main() -> None:
    args = parser.parse_args()
    orca_port: int = args.orca_port
    out_file: str = args.out_file

    csvrows: List[List[str]] = []

    for sched_type in ["dFCFS", "cFCFS", "cfs"]:
        # Check for intermediate result
        tmpfile = f"{sched_type}.{out_file}.tmp"
        try:
            with open(tmpfile) as file:
                reader = csv.reader(file)
                rows = list(reader)
                print(f"Found previous result for {sched_type}")
        except:
            print(f"Starting experiment for {sched_type}...")
            rows = run_sched_exp(orca_port=orca_port, sched_type=sched_type)

        # Add header if not exists
        if len(csvrows) == 0:
            csvrows += rows[0:1]

        # Add non-header rows to csv
        csvrows += rows[1:]

        # Write intermediate result
        with open(tmpfile, mode="w") as file:
            writer = csv.writer(file)
            writer.writerows(rows)

    with open(out_file, mode="w") as file:
        writer = csv.writer(file)
        writer.writerows(csvrows)

    print("Finished!")


if __name__ == "__main__":
    main()
