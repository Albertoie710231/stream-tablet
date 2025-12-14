#pragma once

#include "capture_backend.hpp"
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>

// Forward declarations - use void* to avoid including PipeWire/GLib headers
struct pw_main_loop;
struct pw_context;
struct pw_core;
struct pw_stream;

typedef struct _GDBusConnection GDBusConnection;
typedef struct _GDBusProxy GDBusProxy;

namespace stream_tablet {

class PipeWireCapture : public CaptureBackend {
public:
    PipeWireCapture();
    ~PipeWireCapture() override;

    // Initialize capture (display_name is ignored for PipeWire)
    bool init(const char* display_name = nullptr) override;

    // Shutdown and cleanup
    void shutdown() override;

    // Capture a frame (blocking)
    bool capture_frame(CapturedFrame& frame) override;

    // Get screen dimensions
    int get_width() const override { return m_width; }
    int get_height() const override { return m_height; }

    // Check if initialized
    bool is_initialized() const override { return m_initialized; }

    // Backend name
    const char* get_name() const override { return "PipeWire"; }

    // PipeWire callbacks (public for C callback access)
    void on_stream_state_changed(int old_state, int state, const char* error);
    void on_stream_param_changed(uint32_t id, const void* param);
    void on_stream_process();

private:
    // Portal D-Bus methods
    bool init_dbus();
    bool create_session();
    bool select_sources();
    bool start_capture();
    void cleanup_portal();

    // PipeWire methods
    bool init_pipewire();
    bool connect_stream(uint32_t node_id);
    void cleanup_pipewire();

    // Buffer conversion
    void convert_frame(const uint8_t* src, uint32_t src_format,
                       int width, int height, int stride);

    // D-Bus / Portal state
    GDBusConnection* m_dbus_conn = nullptr;
    GDBusProxy* m_portal_proxy = nullptr;
    std::string m_session_handle;
    std::string m_request_token;
    uint32_t m_pipewire_node = 0;
    int m_pipewire_fd = -1;

    // PipeWire state
    struct pw_main_loop* m_pw_loop = nullptr;
    struct pw_context* m_pw_context = nullptr;
    struct pw_core* m_pw_core = nullptr;
    struct pw_stream* m_pw_stream = nullptr;

    // Frame buffer
    std::vector<uint8_t> m_frame_buffer;
    std::mutex m_frame_mutex;
    std::condition_variable m_frame_cv;
    bool m_frame_ready = false;
    uint64_t m_frame_timestamp = 0;

    // Dimensions and format
    int m_width = 0;
    int m_height = 0;
    uint32_t m_format = 0;

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_stream_ready{false};
};

}  // namespace stream_tablet
