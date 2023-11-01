#!/bin/bash

# This script runs a scheduler along with a workload, collect benchmarks,
# and clean everything up after the workload is finished.

if [ "$#" -lt 2 ]; then
  echo "Usage: $0 <path_to_test_case> <path_to_scheduler> ..."
  echo "Example: $0 tests/custom_ghost_test.cc bazel-bin/fifo_per_cpu_agent --ghost_cpus 0-1" # 0-1 signifies the indices of the cpus you would like to use for ghOSt Google Scheduler TM Copyr
  exit 1
fi

TEST_CASE="$1" # path to workload (test case)
SCHEDULER_BIN="$2" # path to scheduler binary

# Shift the first two args to access the remaining args
shift 2
SCHEDULER_ARGS=("$@")

# Example of calling the scheduler binary with the forwarded arguments:
# "$text2" "${args[@]}"  # This will call the scheduler binary with the additional arguments


# Build the test case
# Make a copy of the BUILD config
cp BUILD BUILD.original

# Add the test case to BUILD
TEST_NAME=$(basename "${TEST_CASE%.*}")
echo "
cc_binary(
    name = \"$TEST_NAME\",
    srcs = [
        \"$TEST_CASE\",
    ],
    copts = compiler_flags,
    deps = [
        \":base\",
        \":ghost\",
    ],
)" >> BUILD

# Build the test case
bazel build -c opt $TEST_NAME

BUILD_STATUS=$?

# Restore original BUILD
mv BUILD.original BUILD

# Exit if build failed
if [ $BUILD_STATUS -ne 0 ]; then
    echo "Bazel build failed"
    exit $BUILD_STATUS
fi

echo "Test case built successfully"

TEST_BIN=bazel-bin/$TEST_NAME

# Run the scheduler in the background
COMMAND="sudo $SCHEDULER_BIN ${SCHEDULER_ARGS[@]}"
echo "> $COMMAND"

tmux new -d -s ghost_scheduler $COMMAND
echo "Running scheduler inside of tmux"

# Run test case
echo "=== Running $TEST_NAME ==="
time $TEST_BIN
echo "=== Finished running $TEST_NAME ==="
TEST_STATUS=$?

echo "Test case finished. Status: $TEST_STATUS"
echo "Killing ghOst scheduler..."

# Check if scheduler is still running
# Returns 0 if running, !=0 otherwise
is_scheduler_running() {
  tmux ls | grep -q ghost_scheduler
  return $?
}

if !is_scheduler_running; then
    echo "Scheduler tmux window is not running."
    echo "This may mean the scheduler crashed unexpectedly."
    echo "Run scripts/cleanup.sh if new test cases are failing."
    exit 1
fi

# tmux send-keys sends the keys to the window (C-c = Ctrl+C = SIGINT)
tmux send-keys -t ghost_scheduler:0 C-c

# Check if signal worked
if is_scheduler_running; then
    echo "Attempt to send SIGINT to scheduler failed."
    echo "Scheduler still running in tmux - please kill manually."
else
    echo "Scheduler exited gracefully"
fi
