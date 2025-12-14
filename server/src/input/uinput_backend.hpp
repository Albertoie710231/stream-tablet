#pragma once

#include <cstdint>
#include <string>

namespace stream_tablet {

// Multi-touch slot state
struct TouchSlot {
    bool active = false;
    int tracking_id = -1;
};

class UInputBackend {
public:
    UInputBackend();
    ~UInputBackend();

    // Initialize uinput devices (stylus + mouse + touch like Weylus)
    bool init(int screen_width, int screen_height);

    // Send stylus event (in_range=false when stylus goes out of proximity)
    void send_stylus(int x, int y, float pressure, float tilt_x, float tilt_y,
                     bool tip_down, bool button1, bool button2, bool eraser,
                     bool in_range = true);

    // Send touch event with pressure
    void send_touch(int x, int y, int slot, bool down, float pressure = 1.0f);

    // Release all pressed buttons and tools (call on disconnect/shutdown)
    void reset_all();

    // Sync events (call after sending events)
    void sync();

    // Shutdown
    void shutdown();

    bool is_initialized() const {
        return m_stylus_fd >= 0 && m_mouse_fd >= 0 && m_touch_fd >= 0;
    }

private:
    void emit(int fd, int type, int code, int value);
    int transform_coord(int val, int max);

    bool init_stylus_device();
    bool init_mouse_device();
    bool init_touch_device();

    void destroy_stylus_device();
    void destroy_mouse_device();
    void destroy_touch_device();

    int m_stylus_fd = -1;  // Stylus/pen device
    int m_mouse_fd = -1;   // Mouse device (for BTN_LEFT/RIGHT/MIDDLE)
    int m_touch_fd = -1;   // Touch device (for multitouch)

    int m_screen_width = 0;
    int m_screen_height = 0;

    // Stylus state
    bool m_stylus_tool_active = false;
    bool m_stylus_touching = false;

    // Touch state (5 slots like Weylus)
    TouchSlot m_touch_slots[5] = {};
};

}  // namespace stream_tablet
