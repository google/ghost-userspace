#!/bin/bash

if [ "$1" == "build" ]; then
    make clean -C orca
    make -C orca
else
    ARGS=("$@")
    sudo ./orca/orca_client ${ARGS[@]}
fi
