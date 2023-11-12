#!/bin/bash

# This script runs a scheduler along with a workload, collect benchmarks,
# and clean everything up after the workload is finished.

if [ "$#" -lt 2 ]; then
  echo "Usage: $0 <path_to_test_case>"
  echo "Example: $0 tests/custom_ghost_test.cc"
  exit 1
fi

TEST_CASE="$1" # path to workload (test case)

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

# Run test case
echo "=== Running $TEST_NAME ==="
time $TEST_BIN
echo "=== Finished running $TEST_NAME ==="
TEST_STATUS=$?

echo "Test case finished. Status: $TEST_STATUS"
