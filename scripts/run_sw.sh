#!/bin/bash

ARGS=("$@")
time bazel-bin/simple_workload ${ARGS[@]}
