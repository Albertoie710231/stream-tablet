#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>
#include <sys/shm.h>

namespace stream_tablet {

struct CapturedFrame {
    uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    uint64_t timestamp_us = 0;
};

class X11Capture {
public:
    X11Capture();
    ~X11Capture();

    // Disable copy
    X11Capture(const X11Capture&) = delete;
    X11Capture& operator=(const X11Capture&) = delete;

    // Initialize capture for the given display
    bool init(const char* display_name = nullptr);

    // Shutdown and cleanup
    void shutdown();

    // Capture a frame (blocking)
    // Returns true if a new frame was captured
    bool capture_frame(CapturedFrame& frame);

    // Get screen dimensions
    int get_width() const { return m_width; }
    int get_height() const { return m_height; }

    // Check if initialized
    bool is_initialized() const { return m_conn != nullptr; }

private:
    bool init_shm();
    bool init_xfixes();
    void cleanup_shm();
    void draw_cursor();

    xcb_connection_t* m_conn = nullptr;
    xcb_screen_t* m_screen = nullptr;
    xcb_window_t m_root = 0;

    // SHM segment
    xcb_shm_seg_t m_shm_seg = 0;
    int m_shm_id = -1;
    uint8_t* m_shm_data = nullptr;
    size_t m_shm_size = 0;

    int m_width = 0;
    int m_height = 0;
    int m_depth = 0;

    // XFixes for cursor
    bool m_xfixes_available = false;
};

}  // namespace stream_tablet
