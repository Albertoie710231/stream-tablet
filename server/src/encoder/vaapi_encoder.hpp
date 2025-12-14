#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include "stream_tablet/config.hpp"

namespace stream_tablet {

struct EncodedFrame {
    std::vector<uint8_t> data;
    uint64_t timestamp_us = 0;
    bool is_keyframe = false;
};

class VAAPIEncoder {
public:
    VAAPIEncoder();
    ~VAAPIEncoder();

    // Disable copy
    VAAPIEncoder(const VAAPIEncoder&) = delete;
    VAAPIEncoder& operator=(const VAAPIEncoder&) = delete;

    // Initialize encoder
    bool init(const EncoderConfig& config);

    // Shutdown
    void shutdown();

    // Encode a BGRA frame
    bool encode(const uint8_t* bgra_data, int width, int height, int stride,
                uint64_t timestamp_us, EncodedFrame& output);

    // Force next frame to be a keyframe
    void request_keyframe() { m_force_keyframe = true; }

    // Update bitrate dynamically
    void set_bitrate(int bitrate);

    // Get encoder info
    int get_width() const { return m_config.width; }
    int get_height() const { return m_config.height; }
    bool is_initialized() const { return m_impl != nullptr; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    EncoderConfig m_config;
    uint64_t m_frame_count = 0;
    bool m_force_keyframe = false;
};

}  // namespace stream_tablet
