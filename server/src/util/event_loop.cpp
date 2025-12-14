#include "event_loop.hpp"
#include "logger.hpp"

namespace stream_tablet {

struct TimerData {
    std::function<void()> callback;
    EventLoop* loop;
};

static void timer_callback(uv_timer_t* handle) {
    TimerData* data = static_cast<TimerData*>(handle->data);
    if (data && data->callback) {
        data->callback();
    }
}

static void timer_close_callback(uv_handle_t* handle) {
    TimerData* data = static_cast<TimerData*>(handle->data);
    delete data;
    delete reinterpret_cast<uv_timer_t*>(handle);
}

EventLoop::EventLoop() = default;

EventLoop::~EventLoop() {
    if (m_loop) {
        uv_loop_close(m_loop);
        delete m_loop;
    }
}

bool EventLoop::init() {
    m_loop = new uv_loop_t;
    if (uv_loop_init(m_loop) != 0) {
        LOG_ERROR("Failed to initialize event loop");
        delete m_loop;
        m_loop = nullptr;
        return false;
    }
    return true;
}

void* EventLoop::add_timer(uint64_t timeout_ms, uint64_t repeat_ms, std::function<void()> callback) {
    if (!m_loop) return nullptr;

    uv_timer_t* timer = new uv_timer_t;
    TimerData* data = new TimerData{std::move(callback), this};

    uv_timer_init(m_loop, timer);
    timer->data = data;

    uv_timer_start(timer, timer_callback, timeout_ms, repeat_ms);

    return timer;
}

void EventLoop::remove_timer(void* handle) {
    if (!handle) return;

    uv_timer_t* timer = static_cast<uv_timer_t*>(handle);
    uv_timer_stop(timer);
    uv_close(reinterpret_cast<uv_handle_t*>(timer), timer_close_callback);
}

void EventLoop::run() {
    if (!m_loop) return;
    m_running = true;
    uv_run(m_loop, UV_RUN_DEFAULT);
    m_running = false;
}

void EventLoop::stop() {
    if (m_loop) {
        uv_stop(m_loop);
    }
}

void EventLoop::run_once() {
    if (m_loop) {
        uv_run(m_loop, UV_RUN_NOWAIT);
    }
}

}  // namespace stream_tablet
