#pragma once

#include <functional>
#include <uv.h>

namespace stream_tablet {

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    // Initialize
    bool init();

    // Add timer callback (returns handle for cancellation)
    void* add_timer(uint64_t timeout_ms, uint64_t repeat_ms, std::function<void()> callback);

    // Remove timer
    void remove_timer(void* handle);

    // Run event loop (blocking)
    void run();

    // Stop event loop
    void stop();

    // Run one iteration (non-blocking)
    void run_once();

    // Get libuv loop
    uv_loop_t* get_loop() { return m_loop; }

private:
    uv_loop_t* m_loop = nullptr;
    bool m_running = false;
};

}  // namespace stream_tablet
