#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    // Create child process (which will run the scheduler)
    pid_t child_pid = fork();
    if (child_pid == -1) {
        printf("failed to create child process\n");
        return 1;
    }

    if (child_pid == 0) {
        // we are the child
        printf("we are the child\n");

        // Run FIFO scheduler
        char* args[] = {"../bazel-bin/fifo_per_cpu_agent", "--ghost_cpus",
                        "0-1"};
        printf("Starting scheduler\n");
        execv(args[0], args);
    } else {
        // we are the parent
        printf("we are the parent\n");

        sleep(5);

        printf("Killing scheduler\n");

        if (kill(child_pid, SIGTERM) == -1) {
            printf("failed to kill child\n");
            return 1;
        }

        int status;
        waitpid(child_pid, &status, 0);

        if (WIFEXITED(status)) {
            printf("child exited with status %d\n", status);
        } else if (WIFSIGNALED(status)) {
            printf("child terminated by signal %d\n", WTERMSIG(status));
        } else {
            printf("child ended in unknown way\n");
        }
    }

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