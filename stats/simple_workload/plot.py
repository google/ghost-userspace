import csv
from typing import List
import pandas as pd
import matplotlib.pyplot as plt


def filter_trials() -> None:
    "Pick the trial with lowest short_99.9_pct latency out of all trial groups."

    with open("results.txt") as file:
        reader = csv.reader(file)
        rows = list(reader)
        idxofshort999pct = rows[0].index("short_99.9_pct")
        newrows: List[List[str]] = [rows[0]]
        i = 1
        while i < len(rows):
            trows = [rows[i + j] for j in range(5)]
            # pick row with lowest short_99.9_pct
            tran = [row[idxofshort999pct] for row in trows]
            minidx = tran.index(min(tran))
            newrows.append(trows[minidx])
            i += 5
    with open("results2.txt", "w") as file:
        writer = csv.writer(file)
        writer.writerows(newrows)


def make_plot() -> None:
    with open("results2.txt") as file:
        reader = csv.reader(file)
        rows = list(reader)

        data = {
            "sched_type": [],
            "preemption_interval_us": [],
            "throughput": [],
            "proportion_long_jobs": [],
            "short_99.9_pct": [],
        }

        idxs = {k: rows[0].index(k) for k in data.keys()}
        for row in rows[1:]:
            if row[idxs["proportion_long_jobs"]] == "0.5":
                continue
            for key, idx in idxs.items():
                try:
                    v = float(row[idx])
                except:
                    v = row[idx]
                data[key].append(v)

        # Create a DataFrame from the data
        df = pd.DataFrame(data)

        # Group the filtered data by sched_type
        grouped = df.groupby("sched_type")

        # Plot the data
        plt.figure(figsize=(10, 6))
        for name, group in grouped:
            plt.plot(
                group["throughput"], group["short_99.9_pct"], marker="o", label=name
            )

        # Add labels and legend
        plt.xlabel("Throughput")
        plt.ylabel("short_99.9_pct")
        plt.title("Throughput vs short_99.9_pct")
        plt.legend()

        # Show the plot
        plt.grid(True)
        plt.show()


def main() -> None:
    make_plot()


if __name__ == "__main__":
    main()
