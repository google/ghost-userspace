#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <functional>
#include <iostream>

pid_t delegate_to_child(std::function<void()> work) {
    pid_t child_pid = fork();
    if (child_pid == -1) {
        perror("fork error");
        exit(1);
    }

    if (child_pid == 0) {
        work();
        exit(0);
    } else {
        return child_pid;
    }

    // unreachable
    exit(1);
}

void terminate_child(pid_t child_pid) {
    if (kill(child_pid, SIGKILL) == -1) {
        perror("failed to kill child");
        exit(1);
    }

    printf("killing child process (pid=%d) ...\n", child_pid);
    int status;
    waitpid(child_pid, &status, 0);

    if (WIFEXITED(status)) {
        printf("child (pid=%d) exited with status %d\n", child_pid, status);
    } else if (WIFSIGNALED(status)) {
        printf("child (pid=%d) terminated by signal %d\n", child_pid,
               WTERMSIG(status));
    } else {
        printf("child (pid=%d) ended in unknown way\n", child_pid);
    }
}

int main(int argc, char* argv[]) {
    // Create child to run enclave cleanup (HACK)
    delegate_to_child([] {
        char* args[] = {"../scripts/cleanup.sh"};
        execv(args[0], args);
        perror("failed to call cleanup.sh");
        exit(1);
    });

    // Create child process (which will run the scheduler)
    pid_t child_pid = delegate_to_child([] {
        // run FIFO scheduler
        char* args[] = {"/usr/bin/sudo", "../bazel-bin/fifo_per_cpu_agent",
                        "--ghost_cpus", "0-1", NULL};
        execv(args[0], args);
        perror("execv failed");
        exit(1);
    });

    printf("Child pid: %d\n", child_pid);

    sleep(5);

    printf("Killing scheduler\n");
    terminate_child(child_pid);
    /**
     * Pseudocode:
     *
     *  Start CFS in ghOst.
     *  While True:
     *      Receive data from bidirectional socket.
     *      Do some computation on data to determine best scheduler.
     *      Switch scheduler if needed.
     *
     *  While Recv(recvbuf, udpsocket):
     *      Handle data from socket.
     *      Switch scheduler if needed.
     */
    // }
}