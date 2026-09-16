#include "cuda_encoder.hpp"
#include "../util/logger.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>
}

#include <cstring>
#include <algorithm>
#include <vector>
#include <string>

// SIMD optimization
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>  // SSE2
#define HAS_SSE2 1
#endif

namespace stream_tablet {

// Private implementation using FFmpeg
struct CUDAEncoder::Impl {
    AVBufferRef* hw_device_ctx = nullptr;
    AVBufferRef* hw_frames_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* sw_frame = nullptr;
    AVFrame* hw_frame = nullptr;
    AVPacket* packet = nullptr;

    ~Impl() {
        if (packet) av_packet_free(&packet);
        if (hw_frame) av_frame_free(&hw_frame);
        if (sw_frame) av_frame_free(&sw_frame);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        if (hw_frames_ctx) av_buffer_unref(&hw_frames_ctx);
        if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
    }
};

CUDAEncoder::CUDAEncoder() : m_impl(new Impl()) {}

CUDAEncoder::~CUDAEncoder() {
    shutdown();
}

// Get list of available CUDA devices
static std::vector<int> get_cuda_devices() {
    std::vector<int> devices;

    // Suppress FFmpeg logging during enumeration (it logs errors for non-existent devices)
    int old_log_level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);

    // Try to enumerate CUDA devices by attempting to create contexts
    // FFmpeg's CUDA hwcontext uses device indices (0, 1, 2, ...)
    for (int i = 0; i < 8; i++) {  // Check up to 8 devices
        AVBufferRef* test_ctx = nullptr;
        char device_str[16];
        snprintf(device_str, sizeof(device_str), "%d", i);

        int ret = av_hwdevice_ctx_create(&test_ctx, AV_HWDEVICE_TYPE_CUDA, device_str, nullptr, 0);
        if (ret >= 0) {
            devices.push_back(i);
            av_buffer_unref(&test_ctx);
        } else {
            // No more devices
            break;
        }
    }

    // Restore log level
    av_log_set_level(old_log_level);

    return devices;
}

// Try to initialize encoder on a specific CUDA device
static bool try_encoder_on_device(int device_index, const char* encoder_name,
                                   const EncoderConfig& config,
                                   AVBufferRef** hw_device_ctx,
                                   AVBufferRef** hw_frames_ctx,
                                   AVCodecContext** codec_ctx) {
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) return false;

    // Try to create CUDA device context
    char device_str[16];
    snprintf(device_str, sizeof(device_str), "%d", device_index);
    int ret = av_hwdevice_ctx_create(hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, device_str, nullptr, 0);
    if (ret < 0) return false;

    // Create codec context
    *codec_ctx = avcodec_alloc_context3(codec);
    if (!*codec_ctx) {
        av_buffer_unref(hw_device_ctx);
        return false;
    }

    // Configure encoder
    (*codec_ctx)->width = config.width;
    (*codec_ctx)->height = config.height;
    (*codec_ctx)->time_base = {1, config.framerate};
    (*codec_ctx)->framerate = {config.framerate, 1};
    (*codec_ctx)->pix_fmt = AV_PIX_FMT_CUDA;
    (*codec_ctx)->gop_size = config.gop_size;
    (*codec_ctx)->max_b_frames = 0;
    // Reduce frame delay
    (*codec_ctx)->delay = 0;
    (*codec_ctx)->thread_count = 1;  // Single thread for lowest latency

    // NVENC-specific settings
    // Use low latency preset for real-time streaming
    av_opt_set((*codec_ctx)->priv_data, "preset", "p4", 0);  // p4 = medium quality, fast
    av_opt_set((*codec_ctx)->priv_data, "tune", "ll", 0);    // ll = low latency
    av_opt_set((*codec_ctx)->priv_data, "zerolatency", "1", 0);

    // Disable B-frames and lookahead for lowest latency
    av_opt_set_int((*codec_ctx)->priv_data, "b_adapt", 0, 0);
    av_opt_set_int((*codec_ctx)->priv_data, "rc-lookahead", 0, 0);

    if (config.quality_mode == QualityMode::HIGH_QUALITY || config.quality_mode == QualityMode::AUTO) {
        // CQP mode - constant quality, variable bitrate
        av_opt_set((*codec_ctx)->priv_data, "rc", "constqp", 0);
        av_opt_set_int((*codec_ctx)->priv_data, "qp", config.cqp, 0);
        // Also set global_quality for codecs that use it
        (*codec_ctx)->global_quality = config.cqp;
        // Higher bitrate cap for quality mode
        (*codec_ctx)->bit_rate = config.bitrate;
        (*codec_ctx)->rc_max_rate = config.bitrate * 2;
        (*codec_ctx)->rc_buffer_size = config.bitrate;

        // Quality preset based on FPS
        if (config.framerate > 90) {
            av_opt_set((*codec_ctx)->priv_data, "preset", "p3", 0);  // faster
        } else if (config.quality_mode == QualityMode::AUTO) {
            av_opt_set((*codec_ctx)->priv_data, "preset", "p4", 0);  // balanced
        } else {
            av_opt_set((*codec_ctx)->priv_data, "preset", "p5", 0);  // higher quality
        }
    } else {
        // CBR mode - constant bitrate for low latency
        (*codec_ctx)->bit_rate = config.bitrate;
        (*codec_ctx)->rc_max_rate = config.bitrate;
        // Smaller buffer = lower latency
        (*codec_ctx)->rc_buffer_size = config.bitrate / config.framerate;
        av_opt_set((*codec_ctx)->priv_data, "rc", "cbr", 0);
        av_opt_set((*codec_ctx)->priv_data, "preset", "p3", 0);  // fast
    }

    // Create HW frames context
    *hw_frames_ctx = av_hwframe_ctx_alloc(*hw_device_ctx);
    if (!*hw_frames_ctx) {
        avcodec_free_context(codec_ctx);
        av_buffer_unref(hw_device_ctx);
        return false;
    }

    AVHWFramesContext* frames_ctx = (AVHWFramesContext*)(*hw_frames_ctx)->data;
    frames_ctx->format = AV_PIX_FMT_CUDA;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = config.width;
    frames_ctx->height = config.height;
    frames_ctx->initial_pool_size = 4;

    ret = av_hwframe_ctx_init(*hw_frames_ctx);
    if (ret < 0) {
        av_buffer_unref(hw_frames_ctx);
        avcodec_free_context(codec_ctx);
        av_buffer_unref(hw_device_ctx);
        return false;
    }

    (*codec_ctx)->hw_frames_ctx = av_buffer_ref(*hw_frames_ctx);

    // Try to open encoder - this is where we find out if the device supports it
    ret = avcodec_open2(*codec_ctx, codec, nullptr);
    if (ret < 0) {
        av_buffer_unref(hw_frames_ctx);
        avcodec_free_context(codec_ctx);
        av_buffer_unref(hw_device_ctx);
        return false;
    }

    return true;
}

bool CUDAEncoder::init(const EncoderConfig& config) {
    m_config = config;

    // Suppress FFmpeg internal logging (only show errors in quiet mode)
    av_log_set_level(AV_LOG_ERROR);

    // Get available CUDA devices
    auto devices = get_cuda_devices();
    if (devices.empty()) {
        LOG_ERROR("No CUDA devices found");
        return false;
    }

    LOG_INFO("Found %zu CUDA device(s), probing for NVENC support...", devices.size());

    // Define codec info
    struct CodecInfo {
        const char* encoder_name;
        const char* display_name;
        uint8_t codec_id;  // 0=AV1, 1=HEVC, 2=H264
    };

    // All available NVENC codecs
    const CodecInfo all_codecs[] = {
        {"av1_nvenc", "AV1", 0},
        {"hevc_nvenc", "HEVC", 1},
        {"h264_nvenc", "H.264", 2}
    };

    // Build list of codecs to try based on requested type
    std::vector<CodecInfo> codecs_to_try;
    switch (config.codec_type) {
        case CodecType::AV1:
            codecs_to_try.push_back(all_codecs[0]);  // AV1 only
            break;
        case CodecType::HEVC:
            codecs_to_try.push_back(all_codecs[1]);  // HEVC only
            break;
        case CodecType::H264:
            codecs_to_try.push_back(all_codecs[2]);  // H264 only
            break;
        case CodecType::AUTO:
        default:
            // Try all in order of preference
            codecs_to_try.push_back(all_codecs[0]);
            codecs_to_try.push_back(all_codecs[1]);
            codecs_to_try.push_back(all_codecs[2]);
            break;
    }

    // Try each encoder on each device
    for (const auto& codec : codecs_to_try) {
        for (int device : devices) {
            LOG_INFO("Trying %s on CUDA device %d...", codec.display_name, device);

            if (try_encoder_on_device(device, codec.encoder_name, config,
                                       &m_impl->hw_device_ctx,
                                       &m_impl->hw_frames_ctx,
                                       &m_impl->codec_ctx)) {
                LOG_INFO("Success! Using %s encoder on CUDA device %d", codec.display_name, device);
                m_actual_codec = codec.codec_id;

                // Allocate frames
                m_impl->sw_frame = av_frame_alloc();
                m_impl->sw_frame->format = AV_PIX_FMT_NV12;
                m_impl->sw_frame->width = config.width;
                m_impl->sw_frame->height = config.height;
                av_frame_get_buffer(m_impl->sw_frame, 32);

                m_impl->hw_frame = av_frame_alloc();
                av_hwframe_get_buffer(m_impl->hw_frames_ctx, m_impl->hw_frame, 0);

                m_impl->packet = av_packet_alloc();

                LOG_INFO("CUDA/NVENC encoder initialized: %dx%d @ %d fps, %d bps, codec=%s",
                         config.width, config.height, config.framerate, config.bitrate, codec.display_name);
                return true;
            }
        }
    }

    LOG_ERROR("No working NVENC encoder found on any CUDA device");
    return false;
}

void CUDAEncoder::shutdown() {
    m_impl.reset(new Impl());
}

// Fast BGRA to NV12 conversion - optimized with SSE2
// (Same implementation as VAAPI encoder)
static void convert_bgra_to_nv12_fast(const uint8_t* bgra, int width, int height, int src_stride,
                                       uint8_t* y_plane, uint8_t* uv_plane,
                                       int y_stride, int uv_stride) {
#ifdef HAS_SSE2
    // Process Y plane - 4 pixels at a time
    for (int y = 0; y < height; y++) {
        const uint8_t* src = bgra + y * src_stride;
        uint8_t* dst_y = y_plane + y * y_stride;

        int x = 0;
        // Process 4 pixels at a time with loop unrolling
        for (; x <= width - 4; x += 4) {
            uint8_t b0 = src[x*4+0], g0 = src[x*4+1], r0 = src[x*4+2];
            uint8_t b1 = src[x*4+4], g1 = src[x*4+5], r1 = src[x*4+6];
            uint8_t b2 = src[x*4+8], g2 = src[x*4+9], r2 = src[x*4+10];
            uint8_t b3 = src[x*4+12], g3 = src[x*4+13], r3 = src[x*4+14];

            dst_y[x+0] = ((66*r0 + 129*g0 + 25*b0 + 128) >> 8) + 16;
            dst_y[x+1] = ((66*r1 + 129*g1 + 25*b1 + 128) >> 8) + 16;
            dst_y[x+2] = ((66*r2 + 129*g2 + 25*b2 + 128) >> 8) + 16;
            dst_y[x+3] = ((66*r3 + 129*g3 + 25*b3 + 128) >> 8) + 16;
        }
        for (; x < width; x++) {
            int b = src[x*4+0], g = src[x*4+1], r = src[x*4+2];
            dst_y[x] = ((66*r + 129*g + 25*b + 128) >> 8) + 16;
        }
    }

    // Process UV plane - sample 2x2 blocks
    for (int y = 0; y < height / 2; y++) {
        const uint8_t* src0 = bgra + (y * 2) * src_stride;
        const uint8_t* src1 = bgra + (y * 2 + 1) * src_stride;
        uint8_t* dst_uv = uv_plane + y * uv_stride;

        for (int x = 0; x < width / 2; x++) {
            int r = src0[x*8+2] + src0[x*8+6] + src1[x*8+2] + src1[x*8+6];
            int g = src0[x*8+1] + src0[x*8+5] + src1[x*8+1] + src1[x*8+5];
            int b = src0[x*8+0] + src0[x*8+4] + src1[x*8+0] + src1[x*8+4];
            r >>= 2; g >>= 2; b >>= 2;

            int u = ((-38*r - 74*g + 112*b + 128) >> 8) + 128;
            int v = ((112*r - 94*g - 18*b + 128) >> 8) + 128;

            dst_uv[x*2+0] = (u < 0) ? 0 : (u > 255) ? 255 : u;
            dst_uv[x*2+1] = (v < 0) ? 0 : (v > 255) ? 255 : v;
        }
    }
#else
    // Fallback scalar implementation
    for (int y = 0; y < height; y++) {
        const uint8_t* src = bgra + y * src_stride;
        uint8_t* dst_y = y_plane + y * y_stride;
        for (int x = 0; x < width; x++) {
            int b = src[x*4+0], g = src[x*4+1], r = src[x*4+2];
            dst_y[x] = ((66*r + 129*g + 25*b + 128) >> 8) + 16;
        }
    }
    for (int y = 0; y < height / 2; y++) {
        const uint8_t* src0 = bgra + (y * 2) * src_stride;
        const uint8_t* src1 = bgra + (y * 2 + 1) * src_stride;
        uint8_t* dst_uv = uv_plane + y * uv_stride;
        for (int x = 0; x < width / 2; x++) {
            int r = (src0[x*8+2] + src0[x*8+6] + src1[x*8+2] + src1[x*8+6]) >> 2;
            int g = (src0[x*8+1] + src0[x*8+5] + src1[x*8+1] + src1[x*8+5]) >> 2;
            int b = (src0[x*8+0] + src0[x*8+4] + src1[x*8+0] + src1[x*8+4]) >> 2;
            int u = ((-38*r - 74*g + 112*b + 128) >> 8) + 128;
            int v = ((112*r - 94*g - 18*b + 128) >> 8) + 128;
            dst_uv[x*2+0] = std::clamp(u, 0, 255);
            dst_uv[x*2+1] = std::clamp(v, 0, 255);
        }
    }
#endif
}

bool CUDAEncoder::encode(const uint8_t* bgra_data, int width, int height, int stride,
                          uint64_t timestamp_us, EncodedFrame& output) {
    if (!m_impl->codec_ctx) {
        return false;
    }

    AVFrame* sw_frame = m_impl->sw_frame;

    // Convert BGRA to NV12 using optimized function
    convert_bgra_to_nv12_fast(bgra_data, width, height, stride,
                               sw_frame->data[0], sw_frame->data[1],
                               sw_frame->linesize[0], sw_frame->linesize[1]);

    sw_frame->pts = m_frame_count++;

    // Upload to GPU
    int ret = av_hwframe_transfer_data(m_impl->hw_frame, sw_frame, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to upload frame to GPU");
        return false;
    }
    m_impl->hw_frame->pts = sw_frame->pts;

    // Force keyframe if requested
    if (m_force_keyframe) {
        m_impl->hw_frame->pict_type = AV_PICTURE_TYPE_I;
        m_impl->hw_frame->flags |= AV_FRAME_FLAG_KEY;
        LOG_INFO("Forcing keyframe for frame %ld", m_frame_count);
        m_force_keyframe = false;
    } else {
        m_impl->hw_frame->pict_type = AV_PICTURE_TYPE_NONE;
        m_impl->hw_frame->flags &= ~AV_FRAME_FLAG_KEY;
    }

    // Send frame to encoder
    ret = avcodec_send_frame(m_impl->codec_ctx, m_impl->hw_frame);
    if (ret < 0) {
        LOG_ERROR("Error sending frame to encoder");
        return false;
    }

    // Receive encoded packet
    ret = avcodec_receive_packet(m_impl->codec_ctx, m_impl->packet);
    if (ret == AVERROR(EAGAIN)) {
        return false;  // Need more frames
    }
    if (ret < 0) {
        LOG_ERROR("Error receiving packet from encoder");
        return false;
    }

    // Copy to output
    output.data.resize(m_impl->packet->size);
    memcpy(output.data.data(), m_impl->packet->data, m_impl->packet->size);
    output.timestamp_us = timestamp_us;
    output.is_keyframe = (m_impl->packet->flags & AV_PKT_FLAG_KEY) != 0;

    av_packet_unref(m_impl->packet);
    return true;
}

void CUDAEncoder::set_bitrate(int bitrate) {
    m_config.bitrate = bitrate;
    // Would need to reinit encoder to change bitrate
}

}  // namespace stream_tablet
