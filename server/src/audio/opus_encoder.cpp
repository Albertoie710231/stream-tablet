#include "opus_encoder.hpp"
#include "util/logger.hpp"

#include <opus/opus.h>
#include <cstring>

namespace stream_tablet {

OpusEncoder::OpusEncoder() = default;

OpusEncoder::~OpusEncoder() {
    shutdown();
}

bool OpusEncoder::init(const OpusConfig& config) {
    if (m_encoder) {
        LOG_WARN("OpusEncoder already initialized");
        return false;
    }

    m_config = config;

    // Calculate frame size in samples per channel
    m_frame_size = (config.sample_rate * config.frame_size_ms) / 1000;

    int error;
    ::OpusEncoder* encoder = opus_encoder_create(
        config.sample_rate,
        config.channels,
        OPUS_APPLICATION_AUDIO,  // Optimized for music/general audio
        &error
    );

    if (error != OPUS_OK || !encoder) {
        LOG_ERROR("Failed to create Opus encoder: %s", opus_strerror(error));
        return false;
    }

    // Configure encoder for low latency
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(config.bitrate));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));  // Balance CPU/quality
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    // Enable/disable FEC
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(config.enable_fec ? 1 : 0));

    // Disable DTX (discontinuous transmission) for consistent latency
    opus_encoder_ctl(encoder, OPUS_SET_DTX(0));

    // Allocate encode buffer (max Opus packet is 1275 bytes per frame)
    m_encode_buffer.resize(4000);  // Extra space for safety

    // Reserve space for input buffer (a few frames worth)
    m_input_buffer.reserve(static_cast<size_t>(m_frame_size * config.channels * 4));

    m_encoder = encoder;

    LOG_INFO("Opus encoder initialized: %dHz, %d channels, %dkbps, %dms frames (%d samples/frame)",
             config.sample_rate, config.channels, config.bitrate / 1000,
             config.frame_size_ms, m_frame_size);

    return true;
}

void OpusEncoder::shutdown() {
    if (m_encoder) {
        opus_encoder_destroy(static_cast<::OpusEncoder*>(m_encoder));
        m_encoder = nullptr;
        m_input_buffer.clear();
        LOG_INFO("Opus encoder shut down");
    }
}

int OpusEncoder::encode(const float* samples, int samples_per_channel,
                        uint64_t timestamp_us, EncodedAudioCallback callback) {
    if (!m_encoder || !callback) {
        return 0;
    }

    // If buffer is empty, record the start timestamp
    if (m_input_buffer.empty()) {
        m_buffer_start_timestamp = timestamp_us;
    }

    // Add incoming samples to buffer (interleaved)
    size_t total_samples = static_cast<size_t>(samples_per_channel) * m_config.channels;
    m_input_buffer.insert(m_input_buffer.end(), samples, samples + total_samples);

    // Calculate how many complete frames we have
    size_t samples_per_frame = static_cast<size_t>(m_frame_size) * m_config.channels;
    int frames_encoded = 0;

    // Encode all complete frames
    while (m_input_buffer.size() >= samples_per_frame) {
        EncodedAudio output;
        if (encode_frame(m_input_buffer.data(), m_frame_size, m_buffer_start_timestamp, output)) {
            callback(output);
            frames_encoded++;
        }

        // Remove encoded samples from buffer
        m_input_buffer.erase(m_input_buffer.begin(), m_input_buffer.begin() + samples_per_frame);

        // Advance timestamp by frame duration
        m_buffer_start_timestamp += static_cast<uint64_t>(m_config.frame_size_ms) * 1000;
    }

    return frames_encoded;
}

bool OpusEncoder::encode_frame(const float* samples, int samples_per_channel,
                               uint64_t timestamp_us, EncodedAudio& output) {
    if (!m_encoder) {
        LOG_ERROR("Opus encoder not initialized");
        return false;
    }

    if (samples_per_channel != m_frame_size) {
        LOG_ERROR("Opus encoder requires exactly %d samples, got %d",
                  m_frame_size, samples_per_channel);
        return false;
    }

    ::OpusEncoder* encoder = static_cast<::OpusEncoder*>(m_encoder);

    // Encode the float samples
    int encoded_bytes = opus_encode_float(
        encoder,
        samples,
        samples_per_channel,
        m_encode_buffer.data(),
        static_cast<int>(m_encode_buffer.size())
    );

    if (encoded_bytes < 0) {
        LOG_ERROR("Opus encode failed: %s", opus_strerror(encoded_bytes));
        return false;
    }

    // Copy encoded data to output
    output.data.assign(m_encode_buffer.begin(), m_encode_buffer.begin() + encoded_bytes);
    output.timestamp_us = timestamp_us;
    output.samples_per_channel = samples_per_channel;

    return true;
}

}  // namespace stream_tablet
