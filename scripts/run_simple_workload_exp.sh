#!/bin/bash

# Start tmux, which will run Orca
tmux new-session -d -s orca_session

restart_orca() {
    tmux send-keys -t orca_session C-c
    tmux send-keys -t orca_session "sudo scripts/cleanup.sh" C-m
    tmux send-keys -t orca_session "sudo orca/orca 8000" C-m
}

restart_orca

for i in {1..10}
do
    scripts/simple_workload_experiment.py --orca_port 8000 --out_file results${i}.txt &
    exp_pid=$!

    # If the experiment is still running after 30 secs, then restart it
    sleep 30
    if kill -0 $exp_pid 2>/dev/null; then
        restart_orca
        kill $exp_pid
    fi

    wait $exp_pid
done
