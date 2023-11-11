#!/bin/bash

# Start from 5, go up to 50, in steps of 5
for i in {5..50..5}
do
  # Run the command 10 times for each number
  for j in {1..10}
  do
    # Use the variables in the command and redirect output to the desired file
    sudo bazel-bin/experiments/scripts/shinjuku_dynamic_interval.par $i | tee "shinjuku_${i}_${j}.txt"
  done
done
