#!/bin/bash

for i in {1..10}; do
    scripts/simple_workload_experiment.py \
        --orca_port 8000 \
        --out_file results${i}.txt
done
