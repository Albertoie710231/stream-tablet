#include "pulseaudio_audio.hpp"
#include "util/logger.hpp"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

#include <chrono>
#include <cstring>

namespace stream_tablet {

PulseAudioAudio::PulseAudioAudio() = default;

PulseAudioAudio::~PulseAudioAudio() {
    shutdown();
}

std::string PulseAudioAudio::find_monitor_source() {
    // Try to find the default sink's monitor source
    // This is typically named "<sink_name>.monitor"
    // For simplicity, we use the default monitor source
    return "";  // Empty string = use default monitor
}

bool PulseAudioAudio::init(const AudioConfig& config) {
    if (m_initialized) {
        LOG_WARN("PulseAudio audio already initialized");
        return false;
    }

    m_config = config;
    m_sample_rate = config.sample_rate;
    m_channels = config.channels;
    m_frame_size = (config.sample_rate * config.frame_size_ms) / 1000;

    // Allocate audio buffer
    m_audio_buffer.resize(static_cast<size_t>(m_frame_size) * m_channels);

    m_initialized = true;
    LOG_INFO("PulseAudio audio initialized: %dHz, %d channels, %dms frames",
             m_sample_rate, m_channels, config.frame_size_ms);
    return true;
}

void PulseAudioAudio::shutdown() {
    stop();
    m_initialized = false;
}

bool PulseAudioAudio::start(AudioCallback callback) {
    if (!m_initialized) {
        LOG_ERROR("PulseAudio audio not initialized");
        return false;
    }

    if (m_capturing) {
        LOG_WARN("PulseAudio audio already capturing");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_callback = callback;
    }

    // Set up PulseAudio sample spec
    pa_sample_spec spec;
    spec.format = PA_SAMPLE_FLOAT32LE;
    spec.rate = static_cast<uint32_t>(m_sample_rate);
    spec.channels = static_cast<uint8_t>(m_channels);

    // Create PulseAudio connection
    int error;

    // Use the default monitor source by specifying "@DEFAULT_MONITOR@"
    // This captures the system audio output
    m_pa = pa_simple_new(
        nullptr,                    // Server (nullptr = default)
        "stream-tablet",            // Application name
        PA_STREAM_RECORD,           // Direction
        "@DEFAULT_MONITOR@",        // Device (monitor of default sink)
        "audio-capture",            // Stream name
        &spec,                      // Sample spec
        nullptr,                    // Channel map (nullptr = default)
        nullptr,                    // Buffer attributes (nullptr = default)
        &error
    );

    if (!m_pa) {
        LOG_ERROR("Failed to connect to PulseAudio: %s", pa_strerror(error));
        return false;
    }

    // Start the capture thread
    m_running = true;
    m_capturing = true;
    m_thread = std::thread(&PulseAudioAudio::capture_thread, this);

    LOG_INFO("PulseAudio audio capture started");
    return true;
}

void PulseAudioAudio::stop() {
    if (!m_capturing && !m_running) {
        return;
    }

    m_running = false;
    m_capturing = false;

    // Wait for thread to finish
    if (m_thread.joinable()) {
        m_thread.join();
    }

    // Close PulseAudio connection
    if (m_pa) {
        pa_simple_free(m_pa);
        m_pa = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_callback = nullptr;
    }

    LOG_INFO("PulseAudio audio capture stopped");
}

void PulseAudioAudio::capture_thread() {
    LOG_INFO("PulseAudio capture thread started");

    const size_t bytes_per_frame = static_cast<size_t>(m_frame_size) * m_channels * sizeof(float);
    std::vector<float> buffer(static_cast<size_t>(m_frame_size) * m_channels);

    while (m_running) {
        int error;

        // Read a frame of audio data
        if (pa_simple_read(m_pa, buffer.data(), bytes_per_frame, &error) < 0) {
            if (m_running) {
                LOG_ERROR("PulseAudio read failed: %s", pa_strerror(error));
            }
            break;
        }

        // Get timestamp
        auto now = std::chrono::high_resolution_clock::now();
        auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();

        // Call the callback with the audio frame
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (m_callback) {
            AudioFrame frame;
            frame.samples = buffer.data();
            frame.num_samples = m_frame_size;
            frame.channels = m_channels;
            frame.timestamp_us = static_cast<uint64_t>(timestamp_us);
            m_callback(frame);
        }
    }

    LOG_INFO("PulseAudio capture thread finished");
}

}  // namespace stream_tablet
