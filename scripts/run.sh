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
$COMMAND &
SCHEDULER_PID=$!
echo "> $COMMAND"
echo "Running scheduler with pid $SCHEDULER_PID"

# Run test case
time $TEST_BIN
TEST_STATUS=$?

echo "Test case finished. Status: $TEST_STATUS"

# Kill scheduler process after test case finishes
sudo kill $SCHEDULER_PID
KILL_STATUS=$?

# Check if kill worked
if [ $KILL_STATUS -ne 0 ]; then
    echo "Failed to kill pid $SCHEDULER_PID."
    echo "This may mean the scheduler failed while running."
    echo "Run scripts/cleanup.sh if new test cases are unexpectedly failing."
    exit $KILL_STATUS
else
    echo "Scheduler exited gracefully"
fi
