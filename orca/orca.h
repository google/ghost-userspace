#pragma once

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "panic.h"

namespace orca {

struct SchedulerConfig {
    enum class SchedulerType { dFCFS, cFCFS };

    SchedulerType type;
    int preemption_interval_us = -1; // ignored for dFCFS
};

class Orca {
public:
    Orca() = default;

    ~Orca() {
        if (curr_sched_pid != 0) {
            terminate_child(curr_sched_pid);
        }
    }

    // Helper which runs a new scheduler and kills the old one (if it exists).
    void set_scheduler(orca::SchedulerConfig config) {
        if (curr_sched_pid != 0) {
            terminate_child(curr_sched_pid);
        }

        curr_sched_pid = run_scheduler(config);
    }

private:
    pid_t curr_sched_pid = 0;

    static pid_t delegate_to_child(std::function<void()> work) {
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

    static void terminate_child(pid_t child_pid) {
        if (kill(child_pid, SIGINT) == -1) {
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
    // This function provides a more friendly C++ wrapper for setting dynamic
    // execv arguments.
    template <size_t MaxNumArgs, size_t MaxStrSize>
    static void set_argbuf(char (&argbuf)[MaxNumArgs][MaxStrSize],
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

    static bool file_exists(const char *filepath) {
        struct stat buf;
        return stat(filepath, &buf) == 0;
    }

    // Run a scheduling agent.
    // Returns the PID of the scheduler.
    static pid_t run_scheduler(orca::SchedulerConfig config) {
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
        /*
        if (file_exists("/sys/fs/ghost/enclave_1")) {
            if (file_exists("/sys/fs/ghost/enclave_2")) {
                // We expect to make one enclave at most, and to keep reusing it
        for
                // each scheduler agent
                // If there is more than one enclave, something went wrong
                panic("more enclaves than expected");
            }
            arglist.push_back("--enclave");
            arglist.push_back("/sys/fs/ghost/enclave_1");
        }
        */

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
};

} // namespace orca
