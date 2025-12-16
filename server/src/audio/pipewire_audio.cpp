#include "pipewire_audio.hpp"
#include "util/logger.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/utils/result.h>
#include <spa/pod/pod.h>

#include <cstring>
#include <chrono>

namespace stream_tablet {

// PipeWire stream callbacks
static void pw_audio_on_state_changed_cb(void* data, enum pw_stream_state old_state,
                                          enum pw_stream_state state, const char* error) {
    auto* audio = static_cast<PipeWireAudio*>(data);
    audio->on_stream_state_changed(static_cast<int>(old_state),
                                    static_cast<int>(state), error);
}

static void pw_audio_on_param_changed_cb(void* data, uint32_t id, const struct spa_pod* param) {
    auto* audio = static_cast<PipeWireAudio*>(data);
    audio->on_stream_param_changed(id, static_cast<const void*>(param));
}

static void pw_audio_on_process_cb(void* data) {
    auto* audio = static_cast<PipeWireAudio*>(data);
    audio->on_stream_process();
}

static struct pw_stream_events audio_stream_events;

static void init_audio_stream_events() {
    memset(&audio_stream_events, 0, sizeof(audio_stream_events));
    audio_stream_events.version = PW_VERSION_STREAM_EVENTS;
    audio_stream_events.state_changed = pw_audio_on_state_changed_cb;
    audio_stream_events.param_changed = pw_audio_on_param_changed_cb;
    audio_stream_events.process = pw_audio_on_process_cb;
}

PipeWireAudio::PipeWireAudio() {
    init_audio_stream_events();
}

PipeWireAudio::~PipeWireAudio() {
    shutdown();
}

bool PipeWireAudio::init(const AudioConfig& config) {
    if (m_initialized) {
        LOG_WARN("PipeWire audio already initialized");
        return false;
    }

    m_config = config;
    m_sample_rate = config.sample_rate;
    m_channels = config.channels;
    m_frame_size = (config.sample_rate * config.frame_size_ms) / 1000;

    // Allocate audio buffer
    m_audio_buffer.resize(static_cast<size_t>(m_frame_size) * m_channels);

    // Initialize PipeWire library
    pw_init(nullptr, nullptr);

    if (!init_pipewire()) {
        LOG_ERROR("Failed to initialize PipeWire");
        pw_deinit();
        return false;
    }

    m_initialized = true;
    LOG_INFO("PipeWire audio initialized: %dHz, %d channels, %dms frames",
             m_sample_rate, m_channels, config.frame_size_ms);
    return true;
}

void PipeWireAudio::shutdown() {
    stop();
    cleanup_pipewire();
    if (m_initialized) {
        pw_deinit();
    }
    m_initialized = false;
}

bool PipeWireAudio::init_pipewire() {
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

    m_pw_core = pw_context_connect(m_pw_context, nullptr, 0);
    if (!m_pw_core) {
        LOG_ERROR("Failed to connect to PipeWire");
        return false;
    }

    return true;
}

bool PipeWireAudio::connect_to_monitor() {
    // Create stream properties to capture from monitor
    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_STREAM_CAPTURE_SINK, "true",  // Capture from sink (monitor)
        PW_KEY_NODE_NAME, "stream-tablet-audio",
        nullptr
    );

    m_pw_stream = pw_stream_new(m_pw_core, "stream-tablet-audio", props);
    if (!m_pw_stream) {
        LOG_ERROR("Failed to create PipeWire audio stream");
        return false;
    }

    // Allocate and set up the stream listener
    m_stream_listener = new struct spa_hook();
    memset(m_stream_listener, 0, sizeof(struct spa_hook));
    pw_stream_add_listener(m_pw_stream, m_stream_listener, &audio_stream_events, this);

    // Build format params - request F32 interleaved stereo at 48kHz
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    const struct spa_pod* params[1];
    struct spa_audio_info_raw audio_info = {};
    audio_info.format = SPA_AUDIO_FORMAT_F32;
    audio_info.rate = static_cast<uint32_t>(m_sample_rate);
    audio_info.channels = static_cast<uint32_t>(m_channels);
    audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
    audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info);

    int ret = pw_stream_connect(
        m_pw_stream,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,  // Auto-connect to default monitor
        static_cast<enum pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS
        ),
        params, 1
    );

    if (ret < 0) {
        LOG_ERROR("Failed to connect audio stream: %s", spa_strerror(ret));
        return false;
    }

    LOG_INFO("Connected to PipeWire audio stream (monitor capture)");
    return true;
}

void PipeWireAudio::cleanup_pipewire() {
    if (m_pw_stream) {
        pw_stream_destroy(m_pw_stream);
        m_pw_stream = nullptr;
    }
    if (m_stream_listener) {
        delete m_stream_listener;
        m_stream_listener = nullptr;
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

bool PipeWireAudio::start(AudioCallback callback) {
    if (!m_initialized) {
        LOG_ERROR("PipeWire audio not initialized");
        return false;
    }

    if (m_capturing) {
        LOG_WARN("PipeWire audio already capturing");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_callback = callback;
    }

    // Connect to monitor
    if (!connect_to_monitor()) {
        LOG_ERROR("Failed to connect to audio monitor");
        return false;
    }

    // Start the stream thread
    m_running = true;
    m_thread = std::thread(&PipeWireAudio::stream_thread, this);

    // Wait for stream to become ready
    int timeout = 50;  // 5 seconds
    while (!m_stream_ready && timeout > 0 && m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        timeout--;
    }

    if (!m_stream_ready) {
        LOG_ERROR("Audio stream failed to start");
        stop();
        return false;
    }

    m_capturing = true;
    LOG_INFO("PipeWire audio capture started");
    return true;
}

void PipeWireAudio::stop() {
    if (!m_capturing && !m_running) {
        return;
    }

    m_running = false;
    m_capturing = false;
    m_stream_ready = false;

    // Signal the main loop to quit
    if (m_pw_loop) {
        pw_main_loop_quit(m_pw_loop);
    }

    // Wait for thread to finish
    if (m_thread.joinable()) {
        m_thread.join();
    }

    // Disconnect and cleanup stream
    if (m_pw_stream) {
        pw_stream_disconnect(m_pw_stream);
        pw_stream_destroy(m_pw_stream);
        m_pw_stream = nullptr;
    }
    if (m_stream_listener) {
        delete m_stream_listener;
        m_stream_listener = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_callback = nullptr;
    }

    LOG_INFO("PipeWire audio capture stopped");
}

void PipeWireAudio::stream_thread() {
    LOG_INFO("PipeWire audio thread started");
    pw_main_loop_run(m_pw_loop);
    LOG_INFO("PipeWire audio thread finished");
}

void PipeWireAudio::on_stream_state_changed(int old_state, int state, const char* error) {
    LOG_INFO("PipeWire audio stream state: %s -> %s",
             pw_stream_state_as_string(static_cast<enum pw_stream_state>(old_state)),
             pw_stream_state_as_string(static_cast<enum pw_stream_state>(state)));

    if (error) {
        LOG_ERROR("Audio stream error: %s", error);
    }

    if (state == PW_STREAM_STATE_STREAMING) {
        m_stream_ready = true;
    } else if (state == PW_STREAM_STATE_ERROR) {
        m_stream_ready = false;
    }
}

void PipeWireAudio::on_stream_param_changed(uint32_t id, const void* param_ptr) {
    const struct spa_pod* param = static_cast<const struct spa_pod*>(param_ptr);
    if (!param || id != SPA_PARAM_Format) {
        return;
    }

    struct spa_audio_info_raw audio_info = {};
    if (spa_format_audio_raw_parse(param, &audio_info) < 0) {
        LOG_ERROR("Failed to parse audio format");
        return;
    }

    m_sample_rate = static_cast<int>(audio_info.rate);
    m_channels = static_cast<int>(audio_info.channels);

    LOG_INFO("Audio format: %dHz, %d channels, format=%d",
             m_sample_rate, m_channels, audio_info.format);
}

void PipeWireAudio::on_stream_process() {
    struct pw_buffer* buf = pw_stream_dequeue_buffer(m_pw_stream);
    if (!buf) {
        return;
    }

    struct spa_data* data = &buf->buffer->datas[0];
    if (!data->data) {
        pw_stream_queue_buffer(m_pw_stream, buf);
        return;
    }

    const float* samples = static_cast<const float*>(data->data);
    int num_samples = static_cast<int>(data->chunk->size / (sizeof(float) * m_channels));

    // Debug: track audio capture
    static int buffer_count = 0;
    static int64_t total_samples = 0;
    static auto last_log = std::chrono::steady_clock::now();
    buffer_count++;
    total_samples += num_samples;

    auto now_log = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now_log - last_log).count() >= 5) {
        // Check if audio is silent (all near-zero samples)
        float max_sample = 0.0f;
        for (int i = 0; i < std::min(num_samples * m_channels, 1000); i++) {
            float abs_val = samples[i] < 0 ? -samples[i] : samples[i];
            if (abs_val > max_sample) max_sample = abs_val;
        }
        LOG_INFO("PipeWire audio: %d buffers, %ld total samples, last buffer %d samples, max_amplitude=%.4f",
                 buffer_count, total_samples, num_samples, max_sample);
        buffer_count = 0;
        total_samples = 0;
        last_log = now_log;
    }

    if (num_samples > 0) {
        // Get timestamp
        auto now = std::chrono::high_resolution_clock::now();
        auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();

        // Call the callback with the audio frame
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (m_callback) {
            AudioFrame frame;
            frame.samples = samples;
            frame.num_samples = num_samples;
            frame.channels = m_channels;
            frame.timestamp_us = static_cast<uint64_t>(timestamp_us);
            m_callback(frame);
        }
    }

    pw_stream_queue_buffer(m_pw_stream, buf);
}

}  // namespace stream_tablet
