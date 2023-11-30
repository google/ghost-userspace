#pragma once

namespace orca {

constexpr size_t MAX_MESSAGE_SIZE = 1024;

struct SchedulerConfig {
    enum class SchedulerType { dFCFS, cFCFS };

    SchedulerType type;
    int preemption_interval_us = -1; // ignored for dFCFS
};

enum class MessageType { SetScheduler };

// Header for all TCP messages to the Orca server.
struct OrcaHeader {
    MessageType type;
};

// Message which results in the scheduler being restarted.
struct OrcaSetScheduler : OrcaHeader {
    SchedulerConfig config;
};

} // namespace orca