#pragma once

#include "capture_backend.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

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

    bool init(const char* display_name = nullptr) override;
    void shutdown() override;
    bool capture_frame(CapturedFrame& frame) override;

    int get_width() const override { return m_width; }
    int get_height() const override { return m_height; }
    bool is_initialized() const override { return m_initialized; }
    const char* get_name() const override { return "PipeWire"; }

    void set_framerate(int fps) override;

    // PipeWire callbacks (public for C callback access)
    void on_stream_state_changed(int old_state, int state, const char* error);
    void on_stream_param_changed(uint32_t id, const void* param);
    void on_stream_process();

private:
    bool init_dbus();
    bool create_session();
    bool select_sources();
    bool start_capture();
    void cleanup_portal();

    bool init_pipewire();
    bool connect_stream(uint32_t node_id);
    void cleanup_pipewire();

    void convert_frame(const uint8_t* src, uint32_t src_format,
                       int width, int height, int stride);

    // Portal session persistence
    void load_restore_token();
    void save_restore_token();

    // D-Bus / Portal state
    GDBusConnection* m_dbus_conn = nullptr;
    GDBusProxy* m_portal_proxy = nullptr;
    std::string m_session_handle;
    std::string m_request_token;
    std::string m_restore_token;  // For skipping picker on subsequent runs
    uint32_t m_pipewire_node = 0;
    int m_pipewire_fd = -1;

    // PipeWire state
    struct pw_main_loop* m_pw_loop = nullptr;
    struct pw_context* m_pw_context = nullptr;
    struct pw_core* m_pw_core = nullptr;
    struct pw_stream* m_pw_stream = nullptr;

    // Double-buffer: on_stream_process writes to latest, capture_frame reads it
    std::vector<uint8_t> m_frame_buffer;  // for format conversion fallback
    struct FrameSlot {
        std::vector<uint8_t> data;
        int stride = 0;
        uint64_t timestamp = 0;
        bool ready = false;
    };
    FrameSlot m_slots[2];
    int m_write_slot = 0;
    int m_last_read_slot = -1;
    std::atomic<bool> m_has_new_frame{false};

    // Dimensions and format
    int m_width = 0;
    int m_height = 0;
    uint32_t m_format = 0;
    int m_target_fps = 60;

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_stream_ready{false};
};

}  // namespace stream_tablet
