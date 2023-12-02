import csv
from dataclasses import dataclass
from decimal import Decimal
import matplotlib.pyplot as plt


@dataclass
class ExpResult:
    sched_type: str
    preemption_interval_us: int
    throughput: int
    proportion_long_jobs: Decimal
    short_99_9_pct: float


def read_csv(filename: str) -> list[ExpResult]:
    "Read CSV into data structure."

    results: list[ExpResult] = []

    with open(filename) as file:
        reader = csv.reader(file)
        rows = list(reader)

        idx_of_keys: dict[str, int] = {rows[0][i]: i for i in range(len(rows[0]))}
        for row in rows[1:]:
            results.append(
                ExpResult(
                    sched_type=row[idx_of_keys["sched_type"]],
                    preemption_interval_us=int(
                        row[idx_of_keys["preemption_interval_us"]]
                    ),
                    throughput=int(row[idx_of_keys["throughput"]]),
                    proportion_long_jobs=Decimal(
                        row[idx_of_keys["proportion_long_jobs"]]
                    ),
                    short_99_9_pct=float(row[idx_of_keys["short_99.9_pct"]]),
                )
            )

    return results


def main() -> None:
    results: list[ExpResult] = []
    for i in range(1, 11):
        results += read_csv(f"results{i}.txt")

    # Want to plot  : throughput vs. short_99_9_pct (tail latency).
    # Multiple lines: sched_type.
    # Fixed vars    : preemption_interval_us, proportion_long_jobs.

    results = [row for row in results if row.proportion_long_jobs == Decimal("0.01")]

    # Ind vars: sched_type, throughput.
    # Dep vars: short_99_9_pct.

    # Filter for lowest tail latency per group.
    m: dict[tuple[str, int], float] = {}
    for row in results:
        k = (row.sched_type, row.throughput)
        v = row.short_99_9_pct
        if k not in m:
            m[k] = v
        else:
            m[k] = min(m[k], v)

    vals: list[tuple[str, int, float]] = [(k[0], k[1], v) for k, v in m.items()]
    for sched_type in ["dFCFS", "cFCFS", "cfs"]:
        svals = [(v[1], v[2]) for v in vals if v[0] == sched_type]
        xs = [v[0] for v in svals]
        ys = [v[1] for v in svals]
        plt.plot(xs, ys, label=sched_type)

    plt.xlabel("Throughput (reqs/sec)")
    plt.ylabel("99.9%% latency")
    plt.title("Scheduler Performance for 99 Short - 1 Long Task Workload")
    plt.legend()
    plt.show()


if __name__ == "__main__":
    main()
