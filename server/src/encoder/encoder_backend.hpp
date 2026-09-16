#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include "stream_tablet/config.hpp"
#include "../capture/capture_backend.hpp"

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

    // Encode a BGRA frame from host memory.
    virtual bool encode(const uint8_t* bgra_data, int width, int height, int stride,
                        uint64_t timestamp_us, EncodedFrame& output) = 0;

    // Encode a captured frame. Backends that support zero-copy override this to
    // import frame.planes directly; the default unwraps the CPU pointer so
    // backends without a GPU import path keep working unchanged.
    virtual bool encode_frame(const CapturedFrame& frame, EncodedFrame& output) {
        if (frame.is_dmabuf || !frame.data) return false;
        return encode(frame.data, frame.width, frame.height, frame.stride,
                      frame.timestamp_us, output);
    }

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

    // Codec configuration record (av1C / hvcC). Empty if the encoder did not
    // produce one. Sent to the client so it can populate csd-0.
    virtual const std::vector<uint8_t>& get_extradata() const {
        static const std::vector<uint8_t> empty;
        return empty;
    }

    // True only when the backend actually built a zero-copy import pipeline.
    // A DMA-BUF capture paired with an encoder that returns false here cannot
    // encode anything, so the caller must renegotiate capture to the CPU path.
    virtual bool uses_dmabuf_input() const { return false; }

protected:
    EncoderBackend() = default;
};

}  // namespace stream_tablet
