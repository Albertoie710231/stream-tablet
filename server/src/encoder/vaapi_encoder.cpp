#include "vaapi_encoder.hpp"
#include "../util/logger.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
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
struct VAAPIEncoder::Impl {
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

VAAPIEncoder::VAAPIEncoder() : m_impl(new Impl()) {}

VAAPIEncoder::~VAAPIEncoder() {
    shutdown();
}

// Get list of available render devices
static std::vector<std::string> get_render_devices() {
    std::vector<std::string> devices;
    DIR* dir = opendir("/dev/dri");
    if (!dir) return devices;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("renderD") == 0) {
            devices.push_back("/dev/dri/" + name);
        }
    }
    closedir(dir);

    // Sort to ensure consistent order (renderD128, renderD129, etc.)
    std::sort(devices.begin(), devices.end());
    return devices;
}

// Try to initialize encoder on a specific device
static bool try_encoder_on_device(const char* device, const char* encoder_name,
                                   const EncoderConfig& config,
                                   AVBufferRef** hw_device_ctx,
                                   AVBufferRef** hw_frames_ctx,
                                   AVCodecContext** codec_ctx) {
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) return false;

    // Try to create device context
    int ret = av_hwdevice_ctx_create(hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, device, nullptr, 0);
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
    (*codec_ctx)->pix_fmt = AV_PIX_FMT_VAAPI;
    (*codec_ctx)->bit_rate = config.bitrate;
    (*codec_ctx)->rc_max_rate = config.bitrate;
    // Smaller buffer = lower latency (1 frame worth of data)
    (*codec_ctx)->rc_buffer_size = config.bitrate / config.framerate;
    (*codec_ctx)->gop_size = config.gop_size;
    (*codec_ctx)->max_b_frames = 0;
    // Reduce frame delay
    (*codec_ctx)->delay = 0;
    (*codec_ctx)->thread_count = 1;  // Single thread for lowest latency

    // Low latency options
    av_opt_set((*codec_ctx)->priv_data, "preset", "fast", 0);
    av_opt_set((*codec_ctx)->priv_data, "tune", "zerolatency", 0);
    av_opt_set((*codec_ctx)->priv_data, "rc_mode", "CBR", 0);
    // VAAPI-specific low latency
    av_opt_set((*codec_ctx)->priv_data, "async_depth", "1", 0);
    av_opt_set_int((*codec_ctx)->priv_data, "idr_interval", config.gop_size, 0);

    // Create HW frames context
    *hw_frames_ctx = av_hwframe_ctx_alloc(*hw_device_ctx);
    if (!*hw_frames_ctx) {
        avcodec_free_context(codec_ctx);
        av_buffer_unref(hw_device_ctx);
        return false;
    }

    AVHWFramesContext* frames_ctx = (AVHWFramesContext*)(*hw_frames_ctx)->data;
    frames_ctx->format = AV_PIX_FMT_VAAPI;
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

bool VAAPIEncoder::init(const EncoderConfig& config) {
    m_config = config;

    // Get available render devices
    auto devices = get_render_devices();
    if (devices.empty()) {
        LOG_ERROR("No render devices found in /dev/dri/");
        return false;
    }

    LOG_INFO("Found %zu render device(s), probing for encoder support...", devices.size());

    // Encoders to try, in order of preference
    const char* encoders[] = {"av1_vaapi", "hevc_vaapi", "h264_vaapi"};
    const char* encoder_names[] = {"AV1", "HEVC", "H.264"};

    // Try each encoder on each device
    for (size_t e = 0; e < sizeof(encoders) / sizeof(encoders[0]); e++) {
        for (const auto& device : devices) {
            LOG_INFO("Trying %s on %s...", encoder_names[e], device.c_str());

            if (try_encoder_on_device(device.c_str(), encoders[e], config,
                                       &m_impl->hw_device_ctx,
                                       &m_impl->hw_frames_ctx,
                                       &m_impl->codec_ctx)) {
                LOG_INFO("Success! Using %s encoder on %s", encoder_names[e], device.c_str());

                // Allocate frames
                m_impl->sw_frame = av_frame_alloc();
                m_impl->sw_frame->format = AV_PIX_FMT_NV12;
                m_impl->sw_frame->width = config.width;
                m_impl->sw_frame->height = config.height;
                av_frame_get_buffer(m_impl->sw_frame, 32);

                m_impl->hw_frame = av_frame_alloc();
                av_hwframe_get_buffer(m_impl->hw_frames_ctx, m_impl->hw_frame, 0);

                m_impl->packet = av_packet_alloc();

                LOG_INFO("VAAPI encoder initialized: %dx%d @ %d fps, %d bps",
                         config.width, config.height, config.framerate, config.bitrate);
                return true;
            }
        }
    }

    LOG_ERROR("No working VAAPI encoder found on any device");
    return false;
}

void VAAPIEncoder::shutdown() {
    m_impl.reset(new Impl());
}

// Fast BGRA to NV12 conversion - optimized with SSE2
static void convert_bgra_to_nv12_fast(const uint8_t* bgra, int width, int height, int src_stride,
                                       uint8_t* y_plane, uint8_t* uv_plane,
                                       int y_stride, int uv_stride) {
#ifdef HAS_SSE2
    // SSE2 constants for RGB to YUV conversion (BT.601)
    // Y = 0.257*R + 0.504*G + 0.098*B + 16
    // U = -0.148*R - 0.291*G + 0.439*B + 128
    // V = 0.439*R - 0.368*G - 0.071*B + 128

    // Process Y plane - 4 pixels at a time
    for (int y = 0; y < height; y++) {
        const uint8_t* src = bgra + y * src_stride;
        uint8_t* dst_y = y_plane + y * y_stride;

        int x = 0;
        // Process 4 pixels at a time with loop unrolling
        for (; x <= width - 4; x += 4) {
            // Extract B, G, R for each pixel directly
            // Pixel layout: B0 G0 R0 A0 B1 G1 R1 A1 B2 G2 R2 A2 B3 G3 R3 A3
            uint8_t b0 = src[x*4+0], g0 = src[x*4+1], r0 = src[x*4+2];
            uint8_t b1 = src[x*4+4], g1 = src[x*4+5], r1 = src[x*4+6];
            uint8_t b2 = src[x*4+8], g2 = src[x*4+9], r2 = src[x*4+10];
            uint8_t b3 = src[x*4+12], g3 = src[x*4+13], r3 = src[x*4+14];

            // Calculate Y values using integer math
            dst_y[x+0] = ((66*r0 + 129*g0 + 25*b0 + 128) >> 8) + 16;
            dst_y[x+1] = ((66*r1 + 129*g1 + 25*b1 + 128) >> 8) + 16;
            dst_y[x+2] = ((66*r2 + 129*g2 + 25*b2 + 128) >> 8) + 16;
            dst_y[x+3] = ((66*r3 + 129*g3 + 25*b3 + 128) >> 8) + 16;
        }
        // Handle remaining pixels
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
            // Average 2x2 block
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

bool VAAPIEncoder::encode(const uint8_t* bgra_data, int width, int height, int stride,
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

void VAAPIEncoder::set_bitrate(int bitrate) {
    m_config.bitrate = bitrate;
    // Would need to reinit encoder to change bitrate
}

}  // namespace stream_tablet
