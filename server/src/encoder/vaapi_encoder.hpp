#pragma once

#include "encoder_backend.hpp"
#include "vaapi_dmabuf_pipeline.hpp"
#include <atomic>
#include <memory>

namespace stream_tablet {

class VAAPIEncoder : public EncoderBackend {
public:
    VAAPIEncoder();
    ~VAAPIEncoder() override;

    // EncoderBackend interface
    bool init(const EncoderConfig& config) override;
    void shutdown() override;
    bool encode(const uint8_t* bgra_data, int width, int height, int stride,
                uint64_t timestamp_us, EncodedFrame& output) override;
    bool encode_frame(const CapturedFrame& frame, EncodedFrame& output) override;
    void request_keyframe() override { m_force_keyframe.store(true, std::memory_order_relaxed); }
    void set_bitrate(int bitrate) override;
    int get_width() const override { return m_config.width; }
    int get_height() const override { return m_config.height; }
    bool is_initialized() const override { return m_impl != nullptr; }
    uint8_t get_codec_type() const override { return m_actual_codec; }
    const char* get_name() const override { return "VAAPI"; }
    const std::vector<uint8_t>& get_extradata() const override { return m_extradata; }
    bool uses_dmabuf_input() const override { return m_dmabuf_active; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    // Submits an already-GPU-resident frame to the encoder and drains one
    // packet. Shared by the CPU-upload and zero-copy paths.
    bool encode_hw_frame(void* av_hw_frame, uint64_t timestamp_us, EncodedFrame& output);

    EncoderConfig m_config;
    uint64_t m_frame_count = 0;
    // Atomic because request_keyframe() is a public entry point with no
    // documented thread affinity, even though the server now only calls it
    // from the capture loop.
    std::atomic<bool> m_force_keyframe{false};
    std::vector<uint8_t> m_extradata;   // codec configuration record (av1C etc.)
    uint8_t m_actual_codec = 0;  // 0=AV1, 1=HEVC, 2=H264

    // Zero-copy path: set when KWin gave us DMA-BUFs and the GPU import
    // pipeline built successfully. Falls back to the CPU path when false.
    VaapiDmaBufPipeline m_dmabuf_pipeline;
    bool m_dmabuf_active = false;
};

}  // namespace stream_tablet
