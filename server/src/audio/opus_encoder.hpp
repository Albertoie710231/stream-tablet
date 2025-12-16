#pragma once

#include <cstdint>
#include <vector>
#include <functional>

namespace stream_tablet {

struct OpusConfig {
    int sample_rate = 48000;
    int channels = 2;
    int bitrate = 128000;      // 128 kbps
    int frame_size_ms = 10;    // 10ms frames for low latency
    bool enable_fec = false;   // Forward error correction (adds latency)
};

struct EncodedAudio {
    std::vector<uint8_t> data;
    uint64_t timestamp_us = 0;
    int samples_per_channel = 0;
};

using EncodedAudioCallback = std::function<void(const EncodedAudio&)>;

class OpusEncoder {
public:
    OpusEncoder();
    ~OpusEncoder();

    // Initialize the encoder with the given configuration
    bool init(const OpusConfig& config);

    // Clean up resources
    void shutdown();

    // Add samples to the buffer and encode complete frames
    // Calls the callback for each encoded frame
    // Returns number of frames encoded
    int encode(const float* samples, int samples_per_channel,
               uint64_t timestamp_us, EncodedAudioCallback callback);

    // Encode a single frame (legacy, for exact frame sizes)
    bool encode_frame(const float* samples, int samples_per_channel,
                      uint64_t timestamp_us, EncodedAudio& output);

    // Get the frame size in samples per channel
    int get_frame_size() const { return m_frame_size; }

    // Check if the encoder is initialized
    bool is_initialized() const { return m_encoder != nullptr; }

    // Get current configuration
    const OpusConfig& get_config() const { return m_config; }

    // Get buffered sample count
    int get_buffered_samples() const { return static_cast<int>(m_input_buffer.size()) / m_config.channels; }

private:
    void* m_encoder = nullptr;  // OpusEncoder* (opaque)

    OpusConfig m_config;
    int m_frame_size = 0;       // samples per channel per frame
    std::vector<uint8_t> m_encode_buffer;
    std::vector<float> m_input_buffer;  // Buffer for accumulating samples
    uint64_t m_buffer_start_timestamp = 0;  // Timestamp of first sample in buffer
};

}  // namespace stream_tablet
