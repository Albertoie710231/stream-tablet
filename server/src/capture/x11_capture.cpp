#include "x11_capture.hpp"
#include "../util/logger.hpp"
#include <xcb/xcb_image.h>
#include <cstring>
#include <chrono>
#include <algorithm>

namespace stream_tablet {

X11Capture::X11Capture() = default;

X11Capture::~X11Capture() {
    shutdown();
}

bool X11Capture::init(const char* display_name) {
    // Connect to X server
    int screen_num = 0;
    m_conn = xcb_connect(display_name, &screen_num);
    if (xcb_connection_has_error(m_conn)) {
        LOG_ERROR("Failed to connect to X server");
        return false;
    }

    // Get screen
    const xcb_setup_t* setup = xcb_get_setup(m_conn);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&iter);
    }
    m_screen = iter.data;
    m_root = m_screen->root;

    m_width = m_screen->width_in_pixels;
    m_height = m_screen->height_in_pixels;
    m_depth = m_screen->root_depth;

    LOG_INFO("Connected to X11 display: %dx%d, depth=%d", m_width, m_height, m_depth);

    // Check SHM extension
    xcb_shm_query_version_cookie_t shm_cookie = xcb_shm_query_version(m_conn);
    xcb_shm_query_version_reply_t* shm_reply = xcb_shm_query_version_reply(m_conn, shm_cookie, nullptr);
    if (!shm_reply) {
        LOG_ERROR("SHM extension not available");
        shutdown();
        return false;
    }
    LOG_INFO("SHM extension version %d.%d", shm_reply->major_version, shm_reply->minor_version);
    free(shm_reply);

    // Initialize shared memory
    if (!init_shm()) {
        shutdown();
        return false;
    }

    // Initialize XFixes for cursor capture
    init_xfixes();

    LOG_INFO("X11 capture initialized successfully");
    return true;
}

bool X11Capture::init_xfixes() {
    // Query XFixes extension
    xcb_xfixes_query_version_cookie_t cookie = xcb_xfixes_query_version(m_conn, 4, 0);
    xcb_xfixes_query_version_reply_t* reply = xcb_xfixes_query_version_reply(m_conn, cookie, nullptr);

    if (reply) {
        LOG_INFO("XFixes extension version %d.%d", reply->major_version, reply->minor_version);
        m_xfixes_available = true;
        free(reply);
        return true;
    }

    LOG_WARN("XFixes extension not available, cursor will not be visible");
    return false;
}

bool X11Capture::init_shm() {
    // Calculate buffer size (BGRA format, 4 bytes per pixel)
    m_shm_size = static_cast<size_t>(m_width) * m_height * 4;

    // Create shared memory segment
    m_shm_id = shmget(IPC_PRIVATE, m_shm_size, IPC_CREAT | 0600);
    if (m_shm_id < 0) {
        LOG_ERROR("Failed to create shared memory segment");
        return false;
    }

    // Attach shared memory
    m_shm_data = static_cast<uint8_t*>(shmat(m_shm_id, nullptr, 0));
    if (m_shm_data == reinterpret_cast<uint8_t*>(-1)) {
        LOG_ERROR("Failed to attach shared memory");
        shmctl(m_shm_id, IPC_RMID, nullptr);
        m_shm_id = -1;
        return false;
    }

    // Attach to X server
    m_shm_seg = xcb_generate_id(m_conn);
    xcb_void_cookie_t cookie = xcb_shm_attach_checked(m_conn, m_shm_seg, m_shm_id, 0);
    xcb_generic_error_t* error = xcb_request_check(m_conn, cookie);
    if (error) {
        LOG_ERROR("Failed to attach SHM to X server: error code %d", error->error_code);
        free(error);
        cleanup_shm();
        return false;
    }

    // Mark segment for deletion after detach
    shmctl(m_shm_id, IPC_RMID, nullptr);

    LOG_INFO("SHM initialized: %zu bytes", m_shm_size);
    return true;
}

void X11Capture::cleanup_shm() {
    if (m_shm_seg && m_conn) {
        xcb_shm_detach(m_conn, m_shm_seg);
        m_shm_seg = 0;
    }
    if (m_shm_data && m_shm_data != reinterpret_cast<uint8_t*>(-1)) {
        shmdt(m_shm_data);
        m_shm_data = nullptr;
    }
}

void X11Capture::shutdown() {
    cleanup_shm();
    if (m_conn) {
        xcb_disconnect(m_conn);
        m_conn = nullptr;
    }
    m_screen = nullptr;
    m_root = 0;
}

bool X11Capture::capture_frame(CapturedFrame& frame) {
    if (!m_conn || !m_shm_data) {
        return false;
    }

    // Get timestamp
    auto now = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    // Capture using SHM
    xcb_shm_get_image_cookie_t cookie = xcb_shm_get_image(
        m_conn,
        m_root,
        0, 0,                    // x, y
        m_width, m_height,       // width, height
        ~0,                      // plane_mask (all planes)
        XCB_IMAGE_FORMAT_Z_PIXMAP,
        m_shm_seg,
        0                        // offset
    );

    xcb_shm_get_image_reply_t* reply = xcb_shm_get_image_reply(m_conn, cookie, nullptr);
    if (!reply) {
        LOG_ERROR("Failed to capture screen");
        return false;
    }
    free(reply);

    // Draw cursor on top of captured frame
    draw_cursor();

    // Fill frame data
    frame.data = m_shm_data;
    frame.width = m_width;
    frame.height = m_height;
    frame.stride = m_width * 4;  // BGRA
    frame.timestamp_us = static_cast<uint64_t>(us);

    return true;
}

void X11Capture::draw_cursor() {
    if (!m_xfixes_available) {
        return;
    }

    // Get cursor image
    xcb_xfixes_get_cursor_image_cookie_t cookie = xcb_xfixes_get_cursor_image(m_conn);
    xcb_xfixes_get_cursor_image_reply_t* cursor = xcb_xfixes_get_cursor_image_reply(m_conn, cookie, nullptr);

    if (!cursor) {
        return;
    }

    // Get cursor data (ARGB format)
    uint32_t* cursor_data = xcb_xfixes_get_cursor_image_cursor_image(cursor);
    int cursor_width = cursor->width;
    int cursor_height = cursor->height;
    int cursor_x = cursor->x - cursor->xhot;
    int cursor_y = cursor->y - cursor->yhot;

    // Composite cursor onto frame buffer
    for (int y = 0; y < cursor_height; y++) {
        int screen_y = cursor_y + y;
        if (screen_y < 0 || screen_y >= m_height) continue;

        for (int x = 0; x < cursor_width; x++) {
            int screen_x = cursor_x + x;
            if (screen_x < 0 || screen_x >= m_width) continue;

            uint32_t pixel = cursor_data[y * cursor_width + x];
            uint8_t a = (pixel >> 24) & 0xFF;
            uint8_t r = (pixel >> 16) & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = pixel & 0xFF;

            if (a == 0) continue;  // Fully transparent

            // Get destination pixel (BGRA format)
            uint8_t* dst = m_shm_data + (screen_y * m_width + screen_x) * 4;

            if (a == 255) {
                // Fully opaque - just copy
                dst[0] = b;
                dst[1] = g;
                dst[2] = r;
                dst[3] = 255;
            } else {
                // Alpha blend
                uint8_t inv_a = 255 - a;
                dst[0] = (b * a + dst[0] * inv_a) / 255;
                dst[1] = (g * a + dst[1] * inv_a) / 255;
                dst[2] = (r * a + dst[2] * inv_a) / 255;
                dst[3] = 255;
            }
        }
    }

    free(cursor);
}

}  // namespace stream_tablet
