#!/bin/bash

ARGS=("$@")
sudo bazel-bin/simple_workload ${ARGS[@]}
