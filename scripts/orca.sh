#!/bin/bash

ORCA_ARGS=("$@")
make clean -C orca
make -C orca
echo "Running orca"
sudo ./orca/orca ${ORC_ARGS[@]}
