#pragma once

#include "capture_backend.hpp"
#include <memory>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>
#include <sys/shm.h>

namespace stream_tablet {

class X11Capture : public CaptureBackend {
public:
    X11Capture();
    ~X11Capture() override;

    // Initialize capture for the given display
    bool init(const char* display_name = nullptr) override;

    // Shutdown and cleanup
    void shutdown() override;

    // Capture a frame (blocking)
    bool capture_frame(CapturedFrame& frame) override;

    // Get screen dimensions
    int get_width() const override { return m_width; }
    int get_height() const override { return m_height; }

    // Check if initialized
    bool is_initialized() const override { return m_conn != nullptr; }

    // Backend name
    const char* get_name() const override { return "X11"; }

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
