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
) -> Tuple[List[List[str]], bool]:
    """Run an experiment.

    Returns the CSV portion of the results,
    as well as whether the experiment timed out.

    """

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
    timed_out = any([line.strip() == "Test timed out." for line in lines])
    csvlines = lines[(lines.index("<csv>") + 1) : lines.index("</csv>")]
    assert len(csvlines) == 2
    return ([csvline.split(", ") for csvline in csvlines], timed_out)


def run_sched_exp(
    orca_port: int, sched_type: str, proportion_long_jobs: Decimal
) -> List[List[str]]:
    "Run the experiment for given scheduler/workload."

    rows: List[List[str]] = []

    preemption_interval_us = 500 if sched_type == "cFCFS" else 0
    set_scheduler(
        orca_port=orca_port,
        sched_type=sched_type,
        preemption_interval_us=preemption_interval_us,
    )

    for throughput in range(20000, 200000 + 1, 20000):
        print(
            f"Starting experiment for sched_type={sched_type} "
            f"throughput={throughput} "
            f"proportion_long_jobs={proportion_long_jobs}"
        )
        (stats, timed_out) = run_experiment(
            sched_type=sched_type,
            throughput=throughput,
            runtime=5,
            num_workers=10,
            proportion_long_jobs=proportion_long_jobs,
        )

        header = [
            "sched_type",
            "preemption_interval_us",
            "throughput",
            "proportion_long_jobs",
        ] + stats[0]

        row = [
            sched_type,
            str(preemption_interval_us),
            str(throughput),
            str(proportion_long_jobs),
        ] + stats[1]

        if len(rows) == 0:
            rows.append(header)
        rows.append(row)

        if timed_out:
            break

    return rows


def main() -> None:
    args = parser.parse_args()
    orca_port: int = args.orca_port
    out_file: str = args.out_file

    csvrows: List[List[str]] = []

    for sched_type in ["dFCFS", "cFCFS", "cfs"]:
        for proportion_long_jobs in [
            Decimal("0"),
            Decimal("0.01"),
            Decimal("0.1"),
            Decimal("0.5"),
        ]:
            # Check for intermediate result
            tmpfile = f"{sched_type}.{proportion_long_jobs}.{out_file}.tmp"
            try:
                with open(tmpfile) as file:
                    reader = csv.reader(file)
                    rows = list(reader)
                    print(f"Found previous result for {sched_type}")
            except:
                print(f"Starting experiment for {sched_type}...")
                rows = run_sched_exp(
                    orca_port=orca_port,
                    sched_type=sched_type,
                    proportion_long_jobs=proportion_long_jobs,
                )

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
