#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

pid_t delegate_to_child(std::function<void()> work) {
    pid_t child_pid = fork();
    if (child_pid == -1) {
        perror("fork");
        exit(1);
    }

    if (child_pid == 0) {
        // set our process group id (pgid) to our own pid
        // this allows our parent to kill us
        if (setpgid(0, 0) == -1) {
            perror("setpgid");
            exit(1);
        }

        work();
        exit(0);
    } else {
        return child_pid;
    }
}

void terminate_child(pid_t child_pid) {
    if (kill(child_pid, SIGTERM) == -1) {
        perror("kill");
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

struct SchedulerConfig {
    enum class SchedulerType { dFCFS, cFCFS };

    SchedulerType type;
    int preemption_interval_us = -1; // ignored for dFCFS
};

// Set args to the arguments pointed to by arglist.
// This function provides a more friendly C++ wrapper for setting dynamic execv
// arguments.
template <size_t MaxNumArgs, size_t MaxStrSize>
void set_argbuf(char (&argbuf)[MaxNumArgs][MaxStrSize],
                const std::vector<std::string> &arglist) {
    if (arglist.size() > MaxNumArgs - 1) {
        perror("too many args in arglist");
        exit(1);
    }
    for (size_t i = 0; i < arglist.size(); ++i) {
        const auto &s = arglist[i];
        if (s.size() > MaxStrSize - 1) {
            perror("arg length too long");
            exit(1);
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
pid_t run_scheduler(SchedulerConfig config) {
    // statically allocate memory for execv args
    // this is kinda sketchy but it should work, since only one scheduling
    // agent runs at a time
    static char argbuf[20][100];

    std::vector<std::string> arglist = {"/usr/bin/sudo"};

    if (config.type == SchedulerConfig::SchedulerType::dFCFS) {
        arglist.push_back("bazel-bin/fifo_per_cpu_agent");
    } else if (config.type == SchedulerConfig::SchedulerType::cFCFS) {
        arglist.push_back("bazel-bin/fifo_centralized_agent");
    } else {
        perror("unrecognized scheduler type");
        exit(1);
    }

    arglist.push_back("--ghost_cpus");
    arglist.push_back("0-7");

    // Check if there is an enclave to attach to
    if (file_exists("/sys/fs/ghost/enclave_1")) {
        if (file_exists("/sys/fs/ghost/enclave_2")) {
            // We expect to make one enclave at most, and to keep reusing it for
            // each scheduler agent
            // If there is more than one enclave, something went wrong
            perror("more enclaves than expected");
            exit(1);
        }
        arglist.push_back("--enclave");
        arglist.push_back("/sys/fs/ghost/enclave_1");
    }

    if (config.type != SchedulerConfig::SchedulerType::dFCFS &&
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

    return delegate_to_child([] {
        execv(args[0], args);
        perror("execv");
        exit(1);
    });
}

int main(int argc, char *argv[]) {
    // temp experiment: switch between dFCFS and cFCFS every second

    SchedulerConfig config;
    config.type = SchedulerConfig::SchedulerType::dFCFS;
    config.preemption_interval_us = 750;

    while (true) {
        if (config.type == SchedulerConfig::SchedulerType::dFCFS) {
            config.type = SchedulerConfig::SchedulerType::cFCFS;
        } else {
            config.type = SchedulerConfig::SchedulerType::dFCFS;
        }

        pid_t child_pid = run_scheduler(config);
        printf("Child pid: %d\n", child_pid);

        sleep(1);

        printf("Killing scheduler\n");
        terminate_child(child_pid);
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