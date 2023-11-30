#!/bin/bash

ARGS=("$@")
make clean -C orca
make -C orca
echo "Running orca"
sudo ./orca/orca ${ARGS[@]}
