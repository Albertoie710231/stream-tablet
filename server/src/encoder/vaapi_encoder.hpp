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

    // Get actual codec used (0=AV1, 1=HEVC, 2=H264)
    uint8_t get_codec_type() const { return m_actual_codec; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    EncoderConfig m_config;
    uint64_t m_frame_count = 0;
    bool m_force_keyframe = false;
    uint8_t m_actual_codec = 0;  // 0=AV1, 1=HEVC, 2=H264
};

}  // namespace stream_tablet
