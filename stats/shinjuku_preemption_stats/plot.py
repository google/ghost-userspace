import re
import matplotlib.pyplot as plt
from dataclasses import dataclass


def extract_throughput(line: str):
    """
    Extracts the throughput value from a line with the format:
    "Running experiment for throughput = <throughput> req/s:"

    Args:
    line (str): Input line containing the throughput information.

    Returns:
    int: Extracted throughput value if found, or None if not found.
    """
    throughput_pattern = r"throughput = (\d+) req/s"
    match = re.search(throughput_pattern, line)
    if match:
        return int(match.group(1))
    else:
        return None


def extract_numbers_from_line(line: str):
    """
    Extracts a vector of 8 numbers from a line with the specified format.

    Args:
    line (str): Input line containing 8 numbers separated by whitespace.

    Returns:
    list of int: List of 8 extracted integers if found, or an empty list if not found.
    """
    numbers = []
    number_pattern = r"\d+"
    matches = re.findall(number_pattern, line)

    if len(matches) == 8:
        numbers = [int(match) for match in matches]

    return numbers


def extract_preemption_time_slice_value(line: str):
    # Define a regular expression pattern to match the argument and its value
    pattern = r"--preemption_time_slice', '(\d+)us'"

    # Use re.search to find the match in the line
    match = re.search(pattern, line)

    # Check if a match was found
    if match:
        # Extract the value as an integer
        preemption_time_slice_value = int(match.group(1))
        return preemption_time_slice_value
    else:
        # Return None if no match found
        return None


@dataclass
class StatsVector:
    total_requests: int
    throughput_req_per_sec: int
    latency_min_us: int
    latency_50pc_us: int
    latency_99pc_us: int
    latency_99_5pc_us: int
    latency_99_9pc_us: int
    latency_max_us: int


@dataclass
class RocksDBOpStats:
    ingress_queue_time: StatsVector
    repeatable_handle_time: StatsVector
    worker_queue_time: StatsVector
    worker_handle_time: StatsVector
    total: StatsVector


@dataclass
class ExperimentStats:
    throughput: int
    get_stats: RocksDBOpStats
    range_stats: RocksDBOpStats
    all_stats: RocksDBOpStats


@dataclass
class PreemptionStats:
    preemption_interval_us: int
    exp_stats: list[ExperimentStats]


def parse_preemption_stats(filepath: str) -> PreemptionStats:
    with open(filepath, "r") as file:
        lines = file.readlines()
        exp_stats: list[ExperimentStats] = []
        preemption_interval_us = None

        i = 0
        while i < len(lines):

            def parse_op_stats_table() -> RocksDBOpStats:
                nonlocal i

                def vec_to_stats_vector(vec: list[int]) -> StatsVector:
                    return StatsVector(
                        total_requests=vec[0],
                        throughput_req_per_sec=vec[1],
                        latency_min_us=vec[2],
                        latency_50pc_us=vec[3],
                        latency_99pc_us=vec[4],
                        latency_99_5pc_us=vec[5],
                        latency_99_9pc_us=vec[6],
                        latency_max_us=vec[7],
                    )

                i += 3
                ingress_queue_time = vec_to_stats_vector(
                    extract_numbers_from_line(lines[i])
                )
                i += 1
                repeatable_handle_time = vec_to_stats_vector(
                    extract_numbers_from_line(lines[i])
                )
                i += 1
                worker_queue_time = vec_to_stats_vector(
                    extract_numbers_from_line(lines[i])
                )
                i += 1
                worker_handle_time = vec_to_stats_vector(
                    extract_numbers_from_line(lines[i])
                )
                i += 1
                total = vec_to_stats_vector(extract_numbers_from_line(lines[i]))
                i += 1
                return RocksDBOpStats(
                    ingress_queue_time,
                    repeatable_handle_time,
                    worker_queue_time,
                    worker_handle_time,
                    total,
                )

            if lines[i].startswith("Running experiment"):
                throughput = extract_throughput(lines[i])
                if throughput is None:
                    raise Exception()
                i += 1
                v = extract_preemption_time_slice_value(lines[i])
                if v is not None:
                    preemption_interval_us = v
                i += 3
                get_stats = parse_op_stats_table()
                range_stats = parse_op_stats_table()
                all_stats = parse_op_stats_table()
                exp_stats.append(
                    ExperimentStats(throughput, get_stats, range_stats, all_stats)
                )
            else:
                i += 1

    if preemption_interval_us is None:
        raise Exception()
    return PreemptionStats(preemption_interval_us, exp_stats)


def plot_preemption_stats(preemption_stats_list: list[PreemptionStats], key: str):
    for preemption_stats in preemption_stats_list:
        preemption_interval_us = preemption_stats.preemption_interval_us
        exp_stats = preemption_stats.exp_stats

        # Initialize lists to store throughput and latency values
        throughputs: list[int] = []
        latencies: list[int] = []

        for exp_stat in exp_stats:
            throughput = exp_stat.throughput
            latency = exp_stat.all_stats.total.__dict__[key]

            throughputs.append(throughput)
            latencies.append(latency)

        plt.plot(
            throughputs,
            latencies,
            label=f"{preemption_interval_us}us",
        )

    # Set plot labels and legend
    plt.xlabel("Throughput (req/s)")
    plt.ylabel(f"{key}")
    plt.legend()
    plt.title(f"{key} vs. Throughput for Different Preemption Intervals")

    # Show the plot
    plt.show()


preemption_stats = [
    parse_preemption_stats(f"shinjuku_{i}.txt") for i in range(5, 35, 5)
]
plot_preemption_stats(preemption_stats, key="latency_99_9pc_us")
