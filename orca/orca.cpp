#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "protocol.h"

void panic(const char *s) {
    perror(s);
    exit(EXIT_FAILURE);
}

pid_t delegate_to_child(std::function<void()> work) {
    pid_t child_pid = fork();
    if (child_pid == -1) {
        panic("fork");
    }

    if (child_pid == 0) {
        // set our process group id (pgid) to our own pid
        // this allows our parent to kill us
        if (setpgid(0, 0) == -1) {
            panic("setpgid");
        }

        work();
        exit(0);
    } else {
        return child_pid;
    }
}

void terminate_child(pid_t child_pid) {
    if (kill(child_pid, SIGTERM) == -1) {
        panic("kill");
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

// Set args to the arguments pointed to by arglist.
// This function provides a more friendly C++ wrapper for setting dynamic execv
// arguments.
template <size_t MaxNumArgs, size_t MaxStrSize>
void set_argbuf(char (&argbuf)[MaxNumArgs][MaxStrSize],
                const std::vector<std::string> &arglist) {
    if (arglist.size() > MaxNumArgs - 1) {
        panic("too many args in arglist");
    }
    for (size_t i = 0; i < arglist.size(); ++i) {
        const auto &s = arglist[i];
        if (s.size() > MaxStrSize - 1) {
            panic("arg length too long");
        }
        strncpy(argbuf[i], s.c_str(), s.size() + 1);
    }
}

bool file_exists(const char *filepath) {
    struct stat buf;
    return stat(filepath, &buf) == 0;
}

// Run a scheduling agent.
// Returns the PID of the scheduler.
pid_t run_scheduler(orca::SchedulerConfig config) {
    // statically allocate memory for execv args
    // this is kinda sketchy but it should work, since only one scheduling
    // agent runs at a time
    static char argbuf[20][100];

    std::vector<std::string> arglist = {"/usr/bin/sudo"};

    if (config.type == orca::SchedulerConfig::SchedulerType::dFCFS) {
        arglist.push_back("bazel-bin/fifo_per_cpu_agent");
    } else if (config.type == orca::SchedulerConfig::SchedulerType::cFCFS) {
        arglist.push_back("bazel-bin/fifo_centralized_agent");
    } else {
        panic("unrecognized scheduler type");
    }

    arglist.push_back("--ghost_cpus");
    arglist.push_back("0-7");

    // Check if there is an enclave to attach to
    if (file_exists("/sys/fs/ghost/enclave_1")) {
        if (file_exists("/sys/fs/ghost/enclave_2")) {
            // We expect to make one enclave at most, and to keep reusing it for
            // each scheduler agent
            // If there is more than one enclave, something went wrong
            panic("more enclaves than expected");
        }
        arglist.push_back("--enclave");
        arglist.push_back("/sys/fs/ghost/enclave_1");
    }

    if (config.type != orca::SchedulerConfig::SchedulerType::dFCFS &&
        config.preemption_interval_us >= 0) {
        arglist.push_back("--preemption_time_slice");
        std::ostringstream ss;
        ss << config.preemption_interval_us << "us";
        arglist.push_back(ss.str());
    }

    set_argbuf(argbuf, arglist);

    static char *args[20];
    memset(args, 0, sizeof(args));
    for (size_t i = 0; i < arglist.size(); ++i) {
        args[i] = argbuf[i];
    }

    // print args to scheduler
    for (size_t i = 0; args[i]; ++i) {
        printf("%s ", args[i]);
    }
    printf("\n");

    return delegate_to_child([] {
        execv(args[0], args);
        panic("execv");
    });
}

// Helper which runs a new scheduler and kills the old one (if it exists).
void replace_scheduler(orca::SchedulerConfig config) {
    static pid_t curr_sched_pid = 0;

    if (curr_sched_pid != 0) {
        terminate_child(curr_sched_pid);
    }

    curr_sched_pid = run_scheduler(config);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return 0;
    }
    int port = atoi(argv[1]);

    // start TCP server for IPC messages
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        panic("socket");
    }

    int yesval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yesval, sizeof(yesval)) ==
        -1) {
        panic("setsockopt");
    }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&saddr, sizeof(saddr)) == -1) {
        panic("bind");
    }

    if (listen(sockfd, 10) == -1) {
        panic("listen");
    }

    printf("Orca listening on port %d...\n", port);
    while (true) {
        int connfd = accept(sockfd, NULL, NULL);
        if (connfd == -1) {
            printf("accept returned -1\n");
            continue;
        }

        char buf[orca::MAX_MESSAGE_SIZE];
        memset(buf, 0, sizeof(buf));

        ssize_t recvd = 0;
        ssize_t rval;
        do {
            rval = recv(connfd, buf + recvd, orca::MAX_MESSAGE_SIZE - recvd, 0);
            if (rval == -1) {
                panic("recv");
            }
            recvd += rval;
        } while (rval > 0);

        auto *header = (orca::OrcaHeader *)buf;
        switch (header->type) {
        case orca::MessageType::SetScheduler: {
            auto *msg = (orca::OrcaSetScheduler *)buf;
            printf(
                "Received SetScheduler. type=%d, preemption_interval_us=%d\n",
                msg->config.type, msg->config.preemption_interval_us);
            replace_scheduler(msg->config);
            break;
        }
        default:
            panic("unimplemented message type");
        }

        close(connfd);
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