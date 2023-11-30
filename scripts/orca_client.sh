#!/bin/bash

ARGS=("$@")
make clean -C orca
make -C orca_client
sudo ./orca/orca_client ${ARGS[@]}
