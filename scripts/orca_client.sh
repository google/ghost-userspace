#!/bin/bash

if [ "$1" == "build" ]; then
    make clean -C orca
    make -C orca
else
    ARGS=("$@")
    ./orca/orca_client ${ARGS[@]}
fi
