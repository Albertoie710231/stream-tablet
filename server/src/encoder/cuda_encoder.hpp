#pragma once

#include "encoder_backend.hpp"
#include <memory>

namespace stream_tablet {

class CUDAEncoder : public EncoderBackend {
public:
    CUDAEncoder();
    ~CUDAEncoder() override;

    // EncoderBackend interface
    bool init(const EncoderConfig& config) override;
    void shutdown() override;
    bool encode(const uint8_t* bgra_data, int width, int height, int stride,
                uint64_t timestamp_us, EncodedFrame& output) override;
    void request_keyframe() override { m_force_keyframe = true; }
    void set_bitrate(int bitrate) override;
    int get_width() const override { return m_config.width; }
    int get_height() const override { return m_config.height; }
    bool is_initialized() const override { return m_impl != nullptr; }
    uint8_t get_codec_type() const override { return m_actual_codec; }
    const char* get_name() const override { return "CUDA"; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    EncoderConfig m_config;
    uint64_t m_frame_count = 0;
    bool m_force_keyframe = false;
    uint8_t m_actual_codec = 0;  // 0=AV1, 1=HEVC, 2=H264
};

}  // namespace stream_tablet
