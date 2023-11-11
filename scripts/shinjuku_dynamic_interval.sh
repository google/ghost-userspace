#!/bin/bash

# Start from 5, go up to 50, in steps of 5
for i in {5..50..5}
do
  # Run the command 10 times for each number
  for j in {1..10}
  do
    # Use the variables in the command and redirect output to the desired file
    FILENAME=shinjuku_$i\_$j.txt
    echo "Writing $FILENAME"
    sudo bazel-bin/experiments/scripts/shinjuku_dynamic_interval.par $i 20000 300000 20000 > ./stats/$FILENAME
  done
done
