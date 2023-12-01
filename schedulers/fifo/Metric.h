#ifndef GHOST_PROFILER_H
#define GHOST_PROFILER_H

#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "lib/base.h"
#include "lib/ghost.h"

#include <inttypes.h>
#include <stdio.h>
namespace ghost
{
    class Metric // Record how long it stayed in that state
    {
    public:
        enum class TaskState
        {
            kCreated,
            kBlocked,
            kRunnable,
            kQueued,
            kOnCpu,
            kYielding,
            kDied,
            unknown
        };

        Gtid gtid;

        absl::Time createdAt;        // created time
        absl::Duration blockTime;    // Blocked state
        absl::Duration runnableTime; // runnable state
        absl::Duration queuedTime;   // Queued state
        absl::Duration onCpuTime;    // OnCPU state
        absl::Duration yieldingTime;
        absl::Time diedAt;
        int64_t preemptCount; // if it's preempted

        TaskState currentState;
        absl::Time stateStarted;

        Metric() {}
        Metric(Gtid _gtid) : gtid(_gtid), createdAt(absl::Now()), currentState(TaskState::kCreated), stateStarted(createdAt) {}
        void updateState(const TaskState newState);
        void printResult(FILE *to);

    private:
        static Metric::TaskState getStateFromString(std::string_view state)
    };
}
#endif