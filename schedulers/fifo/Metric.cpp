#include "schedulers/fifo/Metric.h"

namespace ghost
{
    void Metric::updateState(const Metric::TaskState newState)
    {
        absl::Time currentTime = absl::Now();
        absl::Duration d = currentTime - stateStarted;
        switch (currentState)
        {
        case TaskState::kBlocked:
            blockTime += d;
            break;
        case TaskState::kRunnable:
            runnableTime += d;
            break;
        case TaskState::kQueued:
            queuedTime += d;
            break;
        case TaskState::kOnCpu:
            onCpuTime += d;
            break;
        case TaskState::kYielding:
            yieldingTime += d;
            break;
        default:
            break;
        }
        currentState = newState;
    }

    void Metric::printResult(FILE *to)
    {
        fprintf(to, "=============== Result: tid(%" PRId64 ") ==================\n", gtid.id());
        fprintf(to, "BlockTime: %" PRId64 "\nRunnableTime: %" PRId64 "\nQueuedTime: %" PRId64 "\nonCpuTime: %" PRId64 "\nyieldingTime: %" PRId64 "\n",
                absl::ToInt64Nanoseconds(blockTime), absl::ToInt64Nanoseconds(runnableTime), absl::ToInt64Nanoseconds(queuedTime),
                absl::ToInt64Nanoseconds(onCpuTime), absl::ToInt64Nanoseconds(yieldingTime));
        fprintf(to, "CreatedAt: %" PRId64 ", DiedAt: %" PRId64 "\n", absl::ToUnixSeconds(createdAt), absl::ToUnixSeconds(diedAt));
        fprintf(to, "---------------------------------\n");
    }

    Metric::TaskState Metric::getStateFromString(std::string_view state)
    {
        if (state == "Blocked")
            return Profiler::TaskState::kBlocked;
        else if (state == "Runnable")
            return Profiler::TaskState::kRunnable;
        else if (state == "Queued")
            return Profiler::TaskState::kQueued;
        else if (state == "OnCpu")
            return Profiler::TaskState::kOnCpu;
        else if (state == "yielding")
            return Profiler::TaskState::kYielding;
        else if (state == "Died")
            return Profiler::TaskState::kDied;
        else
        {
            fprintf(stderr, "Task state is unknown(%s)\n", state.data());
            return Profiler::TaskState::unknown;
        }
    }
}