import re
import matplotlib.pyplot as plt
from dataclasses import dataclass
import glob


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


def extract_value(line: str, value_name: str):
    # Define a regular expression pattern to match the argument and its value
    pattern = f"--{value_name}',"
    if value_name == "preemption_time_slice":
        pattern += " '(\d+)us'"
    elif value_name == "range_query_ratio":
        pattern += " '([\d.]+)'"

    # Use re.search to find the match in the line
    match = re.search(r"%s" % pattern, line)

    # Check if a match was found
    if match:
        # Extract the value
        return match.group(1)
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
class Experiment:
    scheduler: str
    preemption_interval_us: int
    range_query_ratio: float
    exp_stats: list[ExperimentStats]


def parse_experiment_result(filepath: str) -> Experiment:
    with open(filepath, "r") as file:
        lines = file.readlines()
        exp_stats: list[ExperimentStats] = []
        preemption_interval_us = None
        range_query_ratio = None
        scheduler = lines[0].split(" ")[1]

        i = 1
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
                try:
                    throughput = extract_throughput(lines[i])
                    if throughput is None:
                        raise Exception()
                    if scheduler == "ghOSt":
                        i += 1
                        v = int(extract_value(lines[i], "preemption_time_slice"))
                        if v is not None:
                            preemption_interval_us = v
                    i += 1
                    range_query_ratio = float(
                        extract_value(lines[i], "range_query_ratio")
                    )

                    i += 2
                    get_stats = parse_op_stats_table()
                    range_stats = parse_op_stats_table()
                    all_stats = parse_op_stats_table()
                    exp_stats.append(
                        ExperimentStats(throughput, get_stats, range_stats, all_stats)
                    )
                except IndexError:
                    break
            else:
                i += 1

    return Experiment(scheduler, preemption_interval_us, range_query_ratio, exp_stats)


def plot_preemption_stats(expr_results: list[Experiment], key: str):
    range_query_ratio = -1
    preemption_interval_us = -1
    for expr in expr_results:
        scheduler_name = expr.scheduler
        range_query_ratio = expr.range_query_ratio
        preemption_interval_us = expr.preemption_interval_us
        exp_stats = expr.exp_stats

        # Initialize lists to store throughput and latency values
        throughputs: list[int] = []
        latencies: list[int] = []

        for exp_stat in exp_stats:
            throughput = exp_stat.throughput
            latency = exp_stat.all_stats.total.__dict__[key]

            throughputs.append(throughput)
            latencies.append(latency)

        label = (
            f"{scheduler_name}_{preemption_interval_us}"
            if scheduler_name == "ghOSt"
            else scheduler_name
        )

        # print(scheduler_name, range_query_ratio, pre)
        plt.plot(
            throughputs,
            latencies,
            # label=f"{scheduler_name}_{range_query_ratio}_{preemption_interval_us}us",
            label=label,
        )

    # Set plot labels and legend
    plt.xlabel("Throughput (req/s)")
    plt.ylabel(f"{key}")
    plt.legend()
    plt.title(
        f"{key} vs. Throughput for Different Preemption Intervals, range_query_ratio: {range_query_ratio}, preempt: {preemption_interval_us}us"
    )

    # Show the plot
    plt.show()


# diff_interval_test_result = glob.glob("../diff_interval_results/*")
# diff_interval_test_result = diff_interval_test_result.sort()
# print(diff_interval_test_result)

# preemption_stats = [parse_experiment_result(file) for file in diff_interval_test_result]

# for item in preemption_stats:
#     print(
#         item.scheduler,
#         item.preemption_interval_us,
#         item.range_query_ratio,
#         item.exp_stats,
#     )
#     break
# plot_preemption_stats(preemption_stats, key="latency_99_9pc_us")
