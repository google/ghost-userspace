#pragma once

#include <functional>
#include <queue>
#include <unordered_map>

#include "helpers.h"

template <typename T> class EventSignal {
public:
    using handle_t = int;

    // Subscribe to the event.
    handle_t sub(std::function<void(T)> callback) {
        handle_t handle = next_handle++;
        listeners[handle] = std::move(callback);
        printf("Registering handle %d\n", handle);
        return handle;
    }

    // Unsubscribe from the event.
    void unsub(handle_t handle) {
        printf("Unregistering handle %d\n", handle);
        auto it = listeners.find(handle);
        if (it == listeners.end()) {
            for (auto &p : listeners) {
                printf("debug %d\n", p.first);
            }
            panic("handle not found in listeners");
        }
        listeners.erase(it);
    }

    // Send an event to all listeners.
    void fire(T value) {
        for (auto &p : listeners) {
            p.second(value);
        }
    }

private:
    std::unordered_map<int, std::function<void(T)>> listeners;

    int next_handle = 0;
};
