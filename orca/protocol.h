#pragma once

#include "orca.h"

namespace orca {

constexpr size_t MAX_MESSAGE_SIZE = 1024;

enum class MessageType { SetScheduler };

// Header for all TCP messages to the Orca server.
struct OrcaHeader {
    MessageType type;
};

// Message which results in the scheduler being restarted.
struct OrcaSetScheduler : OrcaHeader {
    SchedulerConfig config;

    OrcaSetScheduler() { type = MessageType::SetScheduler; }
};

} // namespace orca