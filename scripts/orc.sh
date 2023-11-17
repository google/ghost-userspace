#!/bin/bash

ORC_ARGS=("$@")
make clean -C orchestrator
make -C orchestrator
./orchestrator/orc ${ORC_ARGS[@]}
