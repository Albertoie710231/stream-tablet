#include "pipewire_capture.hpp"
#include "../util/logger.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/result.h>
#include <spa/pod/pod.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <thread>

namespace stream_tablet {

// Portal D-Bus constants
static const char* PORTAL_BUS_NAME = "org.freedesktop.portal.Desktop";
static const char* PORTAL_OBJECT_PATH = "/org/freedesktop/portal/desktop";
static const char* SCREENCAST_INTERFACE = "org.freedesktop.portal.ScreenCast";
static const char* REQUEST_INTERFACE = "org.freedesktop.portal.Request";

// PipeWire stream callbacks
static void pw_on_state_changed_cb(void* data, enum pw_stream_state old_state,
                                    enum pw_stream_state state, const char* error) {
    auto* capture = static_cast<PipeWireCapture*>(data);
    capture->on_stream_state_changed(static_cast<int>(old_state),
                                      static_cast<int>(state), error);
}

static void pw_on_param_changed_cb(void* data, uint32_t id, const struct spa_pod* param) {
    auto* capture = static_cast<PipeWireCapture*>(data);
    capture->on_stream_param_changed(id, static_cast<const void*>(param));
}

static void pw_on_process_cb(void* data) {
    auto* capture = static_cast<PipeWireCapture*>(data);
    capture->on_stream_process();
}

static struct pw_stream_events stream_events;

static void init_stream_events() {
    memset(&stream_events, 0, sizeof(stream_events));
    stream_events.version = PW_VERSION_STREAM_EVENTS;
    stream_events.state_changed = pw_on_state_changed_cb;
    stream_events.param_changed = pw_on_param_changed_cb;
    stream_events.process = pw_on_process_cb;
}

PipeWireCapture::PipeWireCapture() = default;

PipeWireCapture::~PipeWireCapture() {
    shutdown();
}

bool PipeWireCapture::init(const char* /*display_name*/) {
    LOG_INFO("Initializing PipeWire capture via xdg-desktop-portal...");

    pw_init(nullptr, nullptr);
    init_stream_events();

    if (!init_dbus()) {
        LOG_ERROR("Failed to initialize D-Bus connection");
        return false;
    }

    if (!create_session()) {
        LOG_ERROR("Failed to create screencast session");
        cleanup_portal();
        return false;
    }

    if (!select_sources()) {
        LOG_ERROR("Failed to select sources");
        cleanup_portal();
        return false;
    }

    if (!start_capture()) {
        LOG_ERROR("Failed to start capture");
        cleanup_portal();
        return false;
    }

    if (!init_pipewire()) {
        LOG_ERROR("Failed to initialize PipeWire");
        cleanup_portal();
        return false;
    }

    if (!connect_stream(m_pipewire_node)) {
        LOG_ERROR("Failed to connect to PipeWire stream");
        cleanup_pipewire();
        cleanup_portal();
        return false;
    }

    // Wait for stream to become ready
    int timeout = 50;
    while (!m_stream_ready && timeout > 0) {
        pw_loop_iterate(pw_main_loop_get_loop(m_pw_loop), 100);
        timeout--;
    }

    if (!m_stream_ready || m_width == 0 || m_height == 0) {
        LOG_ERROR("Stream failed to initialize or get dimensions");
        cleanup_pipewire();
        cleanup_portal();
        return false;
    }

    m_frame_buffer.resize(static_cast<size_t>(m_width) * m_height * 4);
    m_initialized = true;
    LOG_INFO("PipeWire capture initialized: %dx%d", m_width, m_height);
    return true;
}

void PipeWireCapture::shutdown() {
    m_initialized = false;
    m_stream_ready = false;
    cleanup_pipewire();
    cleanup_portal();
    pw_deinit();
}

bool PipeWireCapture::capture_frame(CapturedFrame& frame) {
    if (!m_initialized) {
        return false;
    }

    // Process PipeWire events — triggers on_stream_process if a frame arrived
    pw_loop_iterate(pw_main_loop_get_loop(m_pw_loop), 0);

    if (!m_has_new_frame.load(std::memory_order_acquire)) {
        pw_loop_iterate(pw_main_loop_get_loop(m_pw_loop), 2);
    }

    if (m_has_new_frame.load(std::memory_order_acquire)) {
        // New frame arrived — update read slot
        m_last_read_slot = 1 - m_write_slot;
        m_has_new_frame.store(false, std::memory_order_release);
    }

    // Return last known good frame (re-encode if static — compresses to almost nothing)
    if (m_last_read_slot < 0) {
        return false;  // No frame ever received
    }

    auto& slot = m_slots[m_last_read_slot];
    if (slot.data.empty()) {
        return false;
    }

    frame.data = slot.data.data();
    frame.stride = slot.stride;
    frame.width = m_width;
    frame.height = m_height;
    frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();

    return true;
}

// ---- D-Bus / Portal methods (unchanged) ----

bool PipeWireCapture::init_dbus() {
    GError* error = nullptr;

    m_dbus_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!m_dbus_conn) {
        LOG_ERROR("Failed to connect to session bus: %s", error->message);
        g_error_free(error);
        return false;
    }

    m_portal_proxy = g_dbus_proxy_new_sync(
        m_dbus_conn,
        G_DBUS_PROXY_FLAGS_NONE,
        nullptr,
        PORTAL_BUS_NAME,
        PORTAL_OBJECT_PATH,
        SCREENCAST_INTERFACE,
        nullptr,
        &error
    );

    if (!m_portal_proxy) {
        LOG_ERROR("Failed to create portal proxy: %s", error->message);
        g_error_free(error);
        return false;
    }

    m_request_token = "stream_tablet_" + std::to_string(getpid());
    return true;
}

static GVariant* wait_for_response(GDBusConnection* conn, const char* request_path, int timeout_ms) {
    GVariant* result = nullptr;
    bool got_response = false;

    auto callback = [](GDBusConnection*, const gchar*, const gchar*, const gchar*,
                       const gchar*, GVariant* parameters, gpointer user_data) {
        auto* data = static_cast<std::pair<GVariant**, bool*>*>(user_data);
        uint32_t response;
        GVariant* results;
        g_variant_get(parameters, "(u@a{sv})", &response, &results);
        if (response == 0) {
            *data->first = g_variant_ref(results);
        }
        g_variant_unref(results);
        *data->second = true;
    };

    std::pair<GVariant**, bool*> callback_data = {&result, &got_response};

    guint signal_id = g_dbus_connection_signal_subscribe(
        conn, PORTAL_BUS_NAME, REQUEST_INTERFACE, "Response",
        request_path, nullptr, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        callback, &callback_data, nullptr
    );

    GMainContext* context = g_main_context_default();
    auto start = std::chrono::steady_clock::now();
    while (!got_response) {
        g_main_context_iteration(context, FALSE);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    g_dbus_connection_signal_unsubscribe(conn, signal_id);
    return result;
}

bool PipeWireCapture::create_session() {
    GError* error = nullptr;

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token",
                          g_variant_new_string(m_request_token.c_str()));
    g_variant_builder_add(&options, "{sv}", "session_handle_token",
                          g_variant_new_string(m_request_token.c_str()));

    GVariant* ret = g_dbus_proxy_call_sync(
        m_portal_proxy, "CreateSession",
        g_variant_new("(a{sv})", &options),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error
    );

    if (!ret) {
        LOG_ERROR("CreateSession failed: %s", error->message);
        g_error_free(error);
        return false;
    }

    const char* request_path;
    g_variant_get(ret, "(o)", &request_path);
    std::string req_path = request_path;
    g_variant_unref(ret);

    GVariant* response = wait_for_response(m_dbus_conn, req_path.c_str(), 30000);
    if (!response) {
        LOG_ERROR("CreateSession timed out or was denied");
        return false;
    }

    const char* session_handle;
    if (g_variant_lookup(response, "session_handle", "&s", &session_handle)) {
        m_session_handle = session_handle;
        LOG_INFO("Created session: %s", m_session_handle.c_str());
    }
    g_variant_unref(response);

    return !m_session_handle.empty();
}

bool PipeWireCapture::select_sources() {
    GError* error = nullptr;

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token",
                          g_variant_new_string(m_request_token.c_str()));
    g_variant_builder_add(&options, "{sv}", "types",
                          g_variant_new_uint32(1));
    g_variant_builder_add(&options, "{sv}", "multiple",
                          g_variant_new_boolean(FALSE));
    g_variant_builder_add(&options, "{sv}", "cursor_mode",
                          g_variant_new_uint32(2));
    // Persist session: 2 = persist until explicitly revoked
    g_variant_builder_add(&options, "{sv}", "persist_mode",
                          g_variant_new_uint32(2));
    // Restore previous session if we have a token (skips picker dialog)
    load_restore_token();
    if (!m_restore_token.empty()) {
        g_variant_builder_add(&options, "{sv}", "restore_token",
                              g_variant_new_string(m_restore_token.c_str()));
        LOG_INFO("Using saved restore token (picker will be skipped)");
    }

    GVariant* ret = g_dbus_proxy_call_sync(
        m_portal_proxy, "SelectSources",
        g_variant_new("(oa{sv})", m_session_handle.c_str(), &options),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error
    );

    if (!ret) {
        LOG_ERROR("SelectSources failed: %s", error->message);
        g_error_free(error);
        return false;
    }

    const char* request_path;
    g_variant_get(ret, "(o)", &request_path);
    std::string req_path = request_path;
    g_variant_unref(ret);

    GVariant* response = wait_for_response(m_dbus_conn, req_path.c_str(), 120000);
    if (!response) {
        LOG_ERROR("SelectSources timed out or was cancelled");
        return false;
    }
    g_variant_unref(response);

    LOG_INFO("Source selected");
    return true;
}

bool PipeWireCapture::start_capture() {
    GError* error = nullptr;

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token",
                          g_variant_new_string(m_request_token.c_str()));

    GVariant* ret = g_dbus_proxy_call_sync(
        m_portal_proxy, "Start",
        g_variant_new("(osa{sv})", m_session_handle.c_str(), "", &options),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error
    );

    if (!ret) {
        LOG_ERROR("Start failed: %s", error->message);
        g_error_free(error);
        return false;
    }

    const char* request_path;
    g_variant_get(ret, "(o)", &request_path);
    std::string req_path = request_path;
    g_variant_unref(ret);

    GVariant* response = wait_for_response(m_dbus_conn, req_path.c_str(), 30000);
    if (!response) {
        LOG_ERROR("Start timed out or was denied");
        return false;
    }

    // Save restore token for next run (skips picker)
    const char* new_token = nullptr;
    if (g_variant_lookup(response, "restore_token", "&s", &new_token) && new_token) {
        m_restore_token = new_token;
        save_restore_token();
        LOG_INFO("Saved restore token for next run");
    }

    GVariant* streams;
    if (g_variant_lookup(response, "streams", "@a(ua{sv})", &streams)) {
        GVariantIter iter;
        g_variant_iter_init(&iter, streams);

        uint32_t node_id;
        GVariant* props;
        if (g_variant_iter_next(&iter, "(u@a{sv})", &node_id, &props)) {
            m_pipewire_node = node_id;
            LOG_INFO("Got PipeWire node: %u", node_id);
            g_variant_unref(props);
        }
        g_variant_unref(streams);
    }

    GUnixFDList* fd_list = nullptr;
    GVariantBuilder opt_builder;
    g_variant_builder_init(&opt_builder, G_VARIANT_TYPE("a{sv}"));

    GVariant* fd_ret = g_dbus_proxy_call_with_unix_fd_list_sync(
        m_portal_proxy, "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", m_session_handle.c_str(), &opt_builder),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fd_list, nullptr, &error
    );

    if (!fd_ret) {
        LOG_ERROR("OpenPipeWireRemote failed: %s", error->message);
        g_error_free(error);
        g_variant_unref(response);
        return false;
    }

    int32_t fd_index;
    g_variant_get(fd_ret, "(h)", &fd_index);
    g_variant_unref(fd_ret);

    if (fd_list) {
        m_pipewire_fd = g_unix_fd_list_get(fd_list, fd_index, nullptr);
        g_object_unref(fd_list);
    }

    g_variant_unref(response);

    if (m_pipewire_fd < 0) {
        LOG_ERROR("Failed to get PipeWire fd");
        return false;
    }

    LOG_INFO("Got PipeWire fd: %d", m_pipewire_fd);
    return m_pipewire_node != 0;
}

void PipeWireCapture::cleanup_portal() {
    if (m_portal_proxy) {
        g_object_unref(m_portal_proxy);
        m_portal_proxy = nullptr;
    }
    if (m_dbus_conn) {
        g_object_unref(m_dbus_conn);
        m_dbus_conn = nullptr;
    }
    if (m_pipewire_fd >= 0) {
        close(m_pipewire_fd);
        m_pipewire_fd = -1;
    }
}

bool PipeWireCapture::init_pipewire() {
    m_pw_loop = pw_main_loop_new(nullptr);
    if (!m_pw_loop) {
        LOG_ERROR("Failed to create PipeWire main loop");
        return false;
    }

    m_pw_context = pw_context_new(pw_main_loop_get_loop(m_pw_loop), nullptr, 0);
    if (!m_pw_context) {
        LOG_ERROR("Failed to create PipeWire context");
        return false;
    }

    m_pw_core = pw_context_connect_fd(m_pw_context, m_pipewire_fd, nullptr, 0);
    if (!m_pw_core) {
        LOG_ERROR("Failed to connect to PipeWire");
        return false;
    }

    m_pipewire_fd = -1;
    return true;
}

bool PipeWireCapture::connect_stream(uint32_t node_id) {
    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        nullptr
    );

    m_pw_stream = pw_stream_new(m_pw_core, "stream-tablet-capture", props);
    if (!m_pw_stream) {
        LOG_ERROR("Failed to create PipeWire stream");
        return false;
    }

    static struct spa_hook stream_listener;
    pw_stream_add_listener(m_pw_stream, &stream_listener, &stream_events, this);

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    struct spa_rectangle size_default = SPA_RECTANGLE(1920, 1080);
    struct spa_rectangle size_min = SPA_RECTANGLE(1, 1);
    struct spa_rectangle size_max = SPA_RECTANGLE(8192, 8192);
    uint32_t target_fps = static_cast<uint32_t>(m_target_fps);
    struct spa_fraction framerate_default = SPA_FRACTION(target_fps, 1);
    struct spa_fraction framerate_min = SPA_FRACTION(0, 1);
    struct spa_fraction framerate_max = SPA_FRACTION(240, 1);

    const struct spa_pod* params[1];
    params[0] = static_cast<const struct spa_pod*>(spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,    SPA_POD_CHOICE_ENUM_Id(5,
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_RGBA,
            SPA_VIDEO_FORMAT_xBGR),
        SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(
            &size_default,
            &size_min,
            &size_max),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &framerate_default,
            &framerate_min,
            &framerate_max)));

    int ret = pw_stream_connect(
        m_pw_stream, PW_DIRECTION_INPUT, node_id,
        static_cast<enum pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS
        ),
        params, 1
    );

    if (ret < 0) {
        LOG_ERROR("Failed to connect stream: %s", spa_strerror(ret));
        return false;
    }

    LOG_INFO("Connected to PipeWire stream, node %u, target %d fps", node_id, m_target_fps);
    return true;
}

void PipeWireCapture::cleanup_pipewire() {
    if (m_pw_stream) {
        pw_stream_destroy(m_pw_stream);
        m_pw_stream = nullptr;
    }
    if (m_pw_core) {
        pw_core_disconnect(m_pw_core);
        m_pw_core = nullptr;
    }
    if (m_pw_context) {
        pw_context_destroy(m_pw_context);
        m_pw_context = nullptr;
    }
    if (m_pw_loop) {
        pw_main_loop_destroy(m_pw_loop);
        m_pw_loop = nullptr;
    }
}

void PipeWireCapture::on_stream_state_changed(int old_state, int state,
                                               const char* error) {
    LOG_INFO("PipeWire stream state: %s -> %s",
             pw_stream_state_as_string(static_cast<enum pw_stream_state>(old_state)),
             pw_stream_state_as_string(static_cast<enum pw_stream_state>(state)));

    if (error) {
        LOG_ERROR("Stream error: %s", error);
    }

    if (state == PW_STREAM_STATE_STREAMING) {
        m_stream_ready = true;
    } else if (state == PW_STREAM_STATE_ERROR) {
        m_stream_ready = false;
        m_initialized = false;
    }
}

void PipeWireCapture::on_stream_param_changed(uint32_t id, const void* param_ptr) {
    const struct spa_pod* param = static_cast<const struct spa_pod*>(param_ptr);
    if (!param || id != SPA_PARAM_Format) return;

    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0) {
        LOG_ERROR("Failed to parse video format");
        return;
    }

    m_width = info.size.width;
    m_height = info.size.height;
    m_format = info.format;

    LOG_INFO("Stream format: %dx%d, format=%d (%s)",
             m_width, m_height, m_format,
             spa_debug_type_find_name(spa_type_video_format, m_format));

    m_frame_buffer.resize(static_cast<size_t>(m_width) * m_height * 4);
}

void PipeWireCapture::on_stream_process() {
    struct pw_buffer* b = pw_stream_dequeue_buffer(m_pw_stream);
    if (!b) return;

    struct spa_buffer* buf = b->buffer;
    struct spa_data* d = &buf->datas[0];

    if (!d->data) {
        pw_stream_queue_buffer(m_pw_stream, b);
        return;
    }

    auto now = std::chrono::high_resolution_clock::now();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    const uint8_t* src = static_cast<const uint8_t*>(d->data);
    int stride = d->chunk->stride ? d->chunk->stride : m_width * 4;

    // Copy into the current write slot
    auto& slot = m_slots[m_write_slot];

    if (m_format == SPA_VIDEO_FORMAT_BGRA || m_format == SPA_VIDEO_FORMAT_BGRx) {
        size_t frame_size = static_cast<size_t>(m_height) * stride;
        if (slot.data.size() != frame_size) {
            slot.data.resize(frame_size);
        }
        memcpy(slot.data.data(), src, frame_size);
        slot.stride = stride;
    } else {
        size_t dst_size = static_cast<size_t>(m_width) * m_height * 4;
        if (slot.data.size() != dst_size) {
            slot.data.resize(dst_size);
        }
        // Use slot data as destination for conversion
        auto saved = std::move(m_frame_buffer);
        m_frame_buffer = std::move(slot.data);
        convert_frame(src, m_format, m_width, m_height, stride);
        slot.data = std::move(m_frame_buffer);
        m_frame_buffer = std::move(saved);
        slot.stride = m_width * 4;
    }

    slot.timestamp = timestamp;
    slot.ready = true;

    // Flip write slot for next frame
    m_write_slot = 1 - m_write_slot;
    m_has_new_frame.store(true, std::memory_order_release);

    pw_stream_queue_buffer(m_pw_stream, b);
}

void PipeWireCapture::convert_frame(const uint8_t* src, uint32_t src_format,
                                     int width, int height, int stride) {
    uint8_t* dst = m_frame_buffer.data();
    int dst_stride = width * 4;

    switch (src_format) {
        case SPA_VIDEO_FORMAT_BGRx:
        case SPA_VIDEO_FORMAT_BGRA:
            if (stride == dst_stride) {
                memcpy(dst, src, static_cast<size_t>(height) * dst_stride);
            } else {
                for (int y = 0; y < height; y++) {
                    memcpy(dst + y * dst_stride, src + y * stride, dst_stride);
                }
            }
            if (src_format == SPA_VIDEO_FORMAT_BGRx) {
                for (int i = 3; i < width * height * 4; i += 4) {
                    dst[i] = 255;
                }
            }
            break;

        case SPA_VIDEO_FORMAT_RGBx:
        case SPA_VIDEO_FORMAT_RGBA:
            for (int y = 0; y < height; y++) {
                const uint8_t* s = src + y * stride;
                uint8_t* d = dst + y * dst_stride;
                for (int x = 0; x < width; x++) {
                    d[x*4 + 0] = s[x*4 + 2];
                    d[x*4 + 1] = s[x*4 + 1];
                    d[x*4 + 2] = s[x*4 + 0];
                    d[x*4 + 3] = (src_format == SPA_VIDEO_FORMAT_RGBA) ? s[x*4 + 3] : 255;
                }
            }
            break;

        case SPA_VIDEO_FORMAT_xBGR:
            for (int y = 0; y < height; y++) {
                const uint8_t* s = src + y * stride;
                uint8_t* d = dst + y * dst_stride;
                for (int x = 0; x < width; x++) {
                    d[x*4 + 0] = s[x*4 + 1];
                    d[x*4 + 1] = s[x*4 + 2];
                    d[x*4 + 2] = s[x*4 + 3];
                    d[x*4 + 3] = 255;
                }
            }
            break;

        default:
            LOG_WARN("Unsupported format %d, copying raw data", src_format);
            if (stride == dst_stride) {
                memcpy(dst, src, static_cast<size_t>(height) * dst_stride);
            } else {
                for (int y = 0; y < height; y++) {
                    memcpy(dst + y * dst_stride, src + y * stride, dst_stride);
                }
            }
            break;
    }
}

void PipeWireCapture::load_restore_token() {
    std::string path = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") +
                       "/.cache/stream-tablet-portal-token";
    FILE* f = fopen(path.c_str(), "r");
    if (f) {
        char buf[512];
        if (fgets(buf, sizeof(buf), f)) {
            m_restore_token = buf;
            // Strip newline
            while (!m_restore_token.empty() && m_restore_token.back() == '\n')
                m_restore_token.pop_back();
        }
        fclose(f);
        LOG_INFO("Loaded restore token from %s", path.c_str());
    }
}

void PipeWireCapture::save_restore_token() {
    std::string path = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") +
                       "/.cache/stream-tablet-portal-token";
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fputs(m_restore_token.c_str(), f);
        fclose(f);
        LOG_INFO("Saved restore token to %s", path.c_str());
    }
}

void PipeWireCapture::set_framerate(int fps) {
    if (fps < 1) fps = 1;
    if (fps > 240) fps = 240;

    // Always reconnect: this is how we pick up resolution changes triggered
    // externally (e.g. kscreen-doctor reconfiguring the virtual output), not
    // just framerate changes.
    LOG_INFO("Reconnecting PipeWire stream at %d fps", fps);
    m_target_fps = fps;

    if (m_pw_stream) {
        pw_stream_disconnect(m_pw_stream);
        pw_stream_destroy(m_pw_stream);
        m_pw_stream = nullptr;
        m_stream_ready = false;

        if (!connect_stream(m_pipewire_node)) {
            LOG_ERROR("Failed to reconnect stream with new framerate");
            return;
        }

        int timeout = 50;
        while (!m_stream_ready && timeout > 0) {
            pw_loop_iterate(pw_main_loop_get_loop(m_pw_loop), 100);
            timeout--;
        }

        if (m_stream_ready) {
            LOG_INFO("PipeWire stream reconnected at %d fps", fps);
        } else {
            LOG_WARN("PipeWire stream reconnect timed out");
        }
    }
}

}  // namespace stream_tablet
