#!/bin/bash

ORC_ARGS=("$@")
make clean -C orchestrator
make -C orchestrator
echo "Running orc"
./orchestrator/orc ${ORC_ARGS[@]}
