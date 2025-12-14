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

    // Initialize PipeWire library
    pw_init(nullptr, nullptr);

    // Initialize stream events struct
    init_stream_events();

    // Initialize D-Bus and portal
    if (!init_dbus()) {
        LOG_ERROR("Failed to initialize D-Bus connection");
        return false;
    }

    // Create screencast session
    if (!create_session()) {
        LOG_ERROR("Failed to create screencast session");
        cleanup_portal();
        return false;
    }

    // Select source (screen/window)
    if (!select_sources()) {
        LOG_ERROR("Failed to select sources");
        cleanup_portal();
        return false;
    }

    // Start capture and get PipeWire node
    if (!start_capture()) {
        LOG_ERROR("Failed to start capture");
        cleanup_portal();
        return false;
    }

    // Initialize PipeWire
    if (!init_pipewire()) {
        LOG_ERROR("Failed to initialize PipeWire");
        cleanup_portal();
        return false;
    }

    // Connect to the stream
    if (!connect_stream(m_pipewire_node)) {
        LOG_ERROR("Failed to connect to PipeWire stream");
        cleanup_pipewire();
        cleanup_portal();
        return false;
    }

    // Wait for stream to become ready and get dimensions
    int timeout = 50;  // 5 seconds
    while (!m_stream_ready && timeout > 0) {
        // Process PipeWire events
        pw_loop_iterate(pw_main_loop_get_loop(m_pw_loop), 100);
        timeout--;
    }

    if (!m_stream_ready || m_width == 0 || m_height == 0) {
        LOG_ERROR("Stream failed to initialize or get dimensions");
        cleanup_pipewire();
        cleanup_portal();
        return false;
    }

    // Allocate frame buffer
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

    // Process PipeWire events (non-blocking)
    pw_loop_iterate(pw_main_loop_get_loop(m_pw_loop), 0);

    // Wait for a frame with timeout
    std::unique_lock<std::mutex> lock(m_frame_mutex);
    if (!m_frame_ready) {
        // Process more events while waiting
        lock.unlock();
        for (int i = 0; i < 10 && !m_frame_ready; i++) {
            pw_loop_iterate(pw_main_loop_get_loop(m_pw_loop), 10);
        }
        lock.lock();
    }

    if (!m_frame_ready) {
        return false;
    }

    // Copy frame data
    frame.data = m_frame_buffer.data();
    frame.width = m_width;
    frame.height = m_height;
    frame.stride = m_width * 4;
    frame.timestamp_us = m_frame_timestamp;
    m_frame_ready = false;

    return true;
}

// D-Bus initialization
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

    // Generate a unique token for request handling
    m_request_token = "stream_tablet_" + std::to_string(getpid());

    return true;
}

// Helper to wait for portal response
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
        conn,
        PORTAL_BUS_NAME,
        REQUEST_INTERFACE,
        "Response",
        request_path,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        callback,
        &callback_data,
        nullptr
    );

    // Wait for response
    GMainContext* context = g_main_context_default();
    auto start = std::chrono::steady_clock::now();
    while (!got_response) {
        g_main_context_iteration(context, FALSE);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) {
            break;
        }
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
        m_portal_proxy,
        "CreateSession",
        g_variant_new("(a{sv})", &options),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error
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

    // Wait for response
    GVariant* response = wait_for_response(m_dbus_conn, req_path.c_str(), 30000);
    if (!response) {
        LOG_ERROR("CreateSession timed out or was denied");
        return false;
    }

    // Get session handle from response
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
    // types: 1=monitor, 2=window, 4=virtual
    g_variant_builder_add(&options, "{sv}", "types",
                          g_variant_new_uint32(1));  // Monitor only
    // multiple: allow multiple sources
    g_variant_builder_add(&options, "{sv}", "multiple",
                          g_variant_new_boolean(FALSE));
    // cursor_mode: 1=hidden, 2=embedded, 4=metadata
    g_variant_builder_add(&options, "{sv}", "cursor_mode",
                          g_variant_new_uint32(2));  // Embedded in stream

    GVariant* ret = g_dbus_proxy_call_sync(
        m_portal_proxy,
        "SelectSources",
        g_variant_new("(oa{sv})", m_session_handle.c_str(), &options),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error
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

    // Wait for user to select source (longer timeout for user interaction)
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
        m_portal_proxy,
        "Start",
        g_variant_new("(osa{sv})", m_session_handle.c_str(), "", &options),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error
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

    // Wait for response
    GVariant* response = wait_for_response(m_dbus_conn, req_path.c_str(), 30000);
    if (!response) {
        LOG_ERROR("Start timed out or was denied");
        return false;
    }

    // Extract PipeWire node ID from streams
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

    // Get PipeWire fd
    GUnixFDList* fd_list = nullptr;
    GVariantBuilder opt_builder;
    g_variant_builder_init(&opt_builder, G_VARIANT_TYPE("a{sv}"));

    GVariant* fd_ret = g_dbus_proxy_call_with_unix_fd_list_sync(
        m_portal_proxy,
        "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", m_session_handle.c_str(), &opt_builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &fd_list,
        nullptr,
        &error
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

    // Take ownership of fd
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

    // Hook for stream events
    static struct spa_hook stream_listener;
    pw_stream_add_listener(m_pw_stream, &stream_listener, &stream_events, this);

    // Build format params - request BGRx or RGBx
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

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
            &SPA_RECTANGLE(1920, 1080),
            &SPA_RECTANGLE(1, 1),
            &SPA_RECTANGLE(8192, 8192)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(60, 1),
            &SPA_FRACTION(0, 1),
            &SPA_FRACTION(144, 1))));

    int ret = pw_stream_connect(
        m_pw_stream,
        PW_DIRECTION_INPUT,
        node_id,
        static_cast<enum pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS
        ),
        params, 1
    );

    if (ret < 0) {
        LOG_ERROR("Failed to connect stream: %s", spa_strerror(ret));
        return false;
    }

    LOG_INFO("Connected to PipeWire stream, node %u", node_id);
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
    if (!param || id != SPA_PARAM_Format) {
        return;
    }

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

    // Resize buffer
    m_frame_buffer.resize(static_cast<size_t>(m_width) * m_height * 4);
}

void PipeWireCapture::on_stream_process() {
    struct pw_buffer* b = pw_stream_dequeue_buffer(m_pw_stream);
    if (!b) {
        return;
    }

    struct spa_buffer* buf = b->buffer;
    struct spa_data* d = &buf->datas[0];

    if (!d->data) {
        pw_stream_queue_buffer(m_pw_stream, b);
        return;
    }

    // Get timestamp
    auto now = std::chrono::high_resolution_clock::now();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    // Convert and copy frame
    const uint8_t* src = static_cast<const uint8_t*>(d->data);
    int stride = d->chunk->stride ? d->chunk->stride : m_width * 4;

    convert_frame(src, m_format, m_width, m_height, stride);

    // Signal frame ready
    {
        std::lock_guard<std::mutex> lock(m_frame_mutex);
        m_frame_timestamp = timestamp;
        m_frame_ready = true;
    }
    m_frame_cv.notify_one();

    pw_stream_queue_buffer(m_pw_stream, b);
}

void PipeWireCapture::convert_frame(const uint8_t* src, uint32_t src_format,
                                     int width, int height, int stride) {
    // Convert to BGRA format (what the encoder expects)
    uint8_t* dst = m_frame_buffer.data();
    int dst_stride = width * 4;

    switch (src_format) {
        case SPA_VIDEO_FORMAT_BGRx:
        case SPA_VIDEO_FORMAT_BGRA:
            // Already BGRA, just copy with potential stride adjustment
            if (stride == dst_stride) {
                memcpy(dst, src, static_cast<size_t>(height) * dst_stride);
            } else {
                for (int y = 0; y < height; y++) {
                    memcpy(dst + y * dst_stride, src + y * stride, dst_stride);
                }
            }
            // Set alpha to 255 for BGRx
            if (src_format == SPA_VIDEO_FORMAT_BGRx) {
                for (int i = 3; i < width * height * 4; i += 4) {
                    dst[i] = 255;
                }
            }
            break;

        case SPA_VIDEO_FORMAT_RGBx:
        case SPA_VIDEO_FORMAT_RGBA:
            // Swap R and B channels
            for (int y = 0; y < height; y++) {
                const uint8_t* s = src + y * stride;
                uint8_t* d = dst + y * dst_stride;
                for (int x = 0; x < width; x++) {
                    d[x*4 + 0] = s[x*4 + 2];  // B <- R
                    d[x*4 + 1] = s[x*4 + 1];  // G <- G
                    d[x*4 + 2] = s[x*4 + 0];  // R <- B
                    d[x*4 + 3] = (src_format == SPA_VIDEO_FORMAT_RGBA) ? s[x*4 + 3] : 255;
                }
            }
            break;

        case SPA_VIDEO_FORMAT_xBGR:
            // xBGR to BGRA: shift bytes
            for (int y = 0; y < height; y++) {
                const uint8_t* s = src + y * stride;
                uint8_t* d = dst + y * dst_stride;
                for (int x = 0; x < width; x++) {
                    d[x*4 + 0] = s[x*4 + 1];  // B
                    d[x*4 + 1] = s[x*4 + 2];  // G
                    d[x*4 + 2] = s[x*4 + 3];  // R
                    d[x*4 + 3] = 255;          // A
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

}  // namespace stream_tablet
