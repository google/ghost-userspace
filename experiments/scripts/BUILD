# Note: If you modify this BUILD file, please contact jhumphri@ first to ensure
# that you are not breaking the Copybara script.

# Runs the RocksDB and Antagonist experiments on ghOSt and CFS (Linux Completely Fair Scheduler).

licenses(["notice"])

load("@subpar//:subpar.bzl", "par_binary")
load("@my_deps//:requirements.bzl", "requirement")

# This library supports all experiments.
py_library(
    name = "experiments",
    srcs = [
        "options.py",
        "run.py",
        "setup.py",
    ],
    data = [
        "//:agent_shinjuku",
        "//:antagonist",
        "//:rocksdb",
    ],
)

# Runs the centralized queuing experiments.
par_binary(
    name = "centralized_queuing",
    srcs = [
        "centralized_queuing.py",
    ],
    python_version = "PY3",
    deps = [
        ":experiments",
        requirement("absl-py"),
    ],
)

# Runs the Shinjuku experiments.
par_binary(
    name = "shinjuku",
    srcs = [
        "shinjuku.py",
    ],
    python_version = "PY3",
    deps = [
        ":experiments",
        requirement("absl-py"),
    ],
)

# Runs the Shenango experiments.
par_binary(
    name = "shenango",
    srcs = [
        "shenango.py",
    ],
    python_version = "PY3",
    deps = [
        ":experiments",
        requirement("absl-py"),
    ],
)

# Runs the Shinjuku+Shenango experiments.
par_binary(
    name = "shinjuku_shenango",
    srcs = [
        "shinjuku_shenango.py",
    ],
    python_version = "PY3",
    deps = [
        ":experiments",
        requirement("absl-py"),
    ],
)

# Run shinjuku_dynamic_interval experiment.
par_binary(
    name = "shinjuku_dynamic_interval",
    srcs = [
        "shinjuku_dynamic_interval.py",
    ],
    python_version = "PY3",
    deps = [
        ":experiments",
        requirement("absl-py"),
    ],
)
