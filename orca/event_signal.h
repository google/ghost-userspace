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
        handle_t handle = next_handle();
        listeners[handle] = std::move(callback);
        return handle;
    }

    // Subscribe to an event; once fired, callback is automatically unsubbed.
    void once(std::function<void(T)> callback) {
        listeners_once.push(std::move(callback));
    }

    // Unsubscribe from the event.
    void unsub(handle_t handle) {
        auto it = listeners.find(handle);
        if (it == listeners.end()) {
            panic("handle not found in listeners");
        }
        listeners.erase(it);
    }

    // Send an event to all listeners.
    void fire(T value) {
        for (auto &p : listeners) {
            p.second(value);
        }
        while (!listeners_once.empty()) {
            auto &callback = listeners_once.front();
            callback(value);
            listeners_once.pop();
        }
    }

private:
    std::unordered_map<int, std::function<void(T)>> listeners;
    std::queue<std::function<void(T)>> listeners_once;

    int next_handle_v = 0;
    handle_t next_handle() { return next_handle_v++; }
};
