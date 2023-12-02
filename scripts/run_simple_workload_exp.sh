#!/bin/bash

# very very bad security
# this is my VM user's password
PW=pass

restart_orca() {
    tmux kill-session -t orca
    tmux new-session -d -s orca
    tmux send-keys -t orca "echo $PW | sudo -S scripts/cleanup.sh; sudo orca/orca 8000" C-m
    sleep 1
}

restart_orca

for i in {1..10}; do
    scripts/simple_workload_experiment.py \
        --orca_port 8000 \
        --out_file results${i}.txt \
        tee "stdout.txt" \
        &
    exp_pid=$!

    # Start timeout countdown
    timeout=30
    (sleep $timeout && kill -KILL $exp_pid) &
    timeout_pid=$!

    while kill -0 $exp_pid; do
        last_printed=$(stat -c %Y stdout.txt)
        now=$(date +%s)
        elapsed=$((now - last_printed))

        if [ $elapsed -ge $timeout ]; then
            kill -KILL $exp_pid
            break
        fi

        sleep 1
    done

    wait $exp_pid
done
