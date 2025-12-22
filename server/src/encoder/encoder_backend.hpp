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

class EncoderBackend {
public:
    virtual ~EncoderBackend() = default;

    // Disable copy
    EncoderBackend(const EncoderBackend&) = delete;
    EncoderBackend& operator=(const EncoderBackend&) = delete;

    // Initialize encoder
    virtual bool init(const EncoderConfig& config) = 0;

    // Shutdown
    virtual void shutdown() = 0;

    // Encode a BGRA frame
    virtual bool encode(const uint8_t* bgra_data, int width, int height, int stride,
                        uint64_t timestamp_us, EncodedFrame& output) = 0;

    // Force next frame to be a keyframe
    virtual void request_keyframe() = 0;

    // Update bitrate dynamically
    virtual void set_bitrate(int bitrate) = 0;

    // Get encoder info
    virtual int get_width() const = 0;
    virtual int get_height() const = 0;
    virtual bool is_initialized() const = 0;

    // Get actual codec used (0=AV1, 1=HEVC, 2=H264)
    virtual uint8_t get_codec_type() const = 0;

    // Get backend name (e.g., "VAAPI", "CUDA")
    virtual const char* get_name() const = 0;

protected:
    EncoderBackend() = default;
};

}  // namespace stream_tablet
