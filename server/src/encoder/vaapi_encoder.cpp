#include "vaapi_encoder.hpp"
#include "../util/logger.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>
}

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <cstdlib>
#include <chrono>
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
                                   AVCodecContext** codec_ctx,
                                   VaapiDmaBufPipeline* dmabuf_pipeline,
                                   bool* dmabuf_active) {
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) return false;

    // Try to create device context
    int ret = av_hwdevice_ctx_create(hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, device, nullptr, 0);
    if (ret < 0) return false;

    // Zero-copy input: build the DMA-BUF -> VAAPI -> NV12 pipeline on this same
    // device. If it fails for any reason we silently keep the CPU path, so a
    // driver that can't import the negotiated modifier degrades instead of
    // breaking the stream.
    if (dmabuf_active) *dmabuf_active = false;
    if (dmabuf_pipeline && config.dmabuf_input) {
        if (dmabuf_pipeline->init(*hw_device_ctx, device, config.width, config.height,
                                  config.drm_format, config.drm_modifier,
                                  config.framerate)) {
            if (dmabuf_active) *dmabuf_active = true;
        } else {
            dmabuf_pipeline->shutdown();
        }
    }

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
    (*codec_ctx)->gop_size = config.gop_size;
    (*codec_ctx)->max_b_frames = 0;
    // Reduce frame delay
    (*codec_ctx)->delay = 0;
    (*codec_ctx)->thread_count = 1;  // Single thread for lowest latency

    // VAAPI-specific settings.
    // async_depth trades latency for throughput. At 1 the encode is fully
    // synchronous: avcodec_send_frame blocks ~8ms on a 2960x1848 AV1 frame,
    // on the capture loop's critical path. At 2 the GPU works in the
    // background and send drops to ~0.15ms, at the cost of one frame of
    // output latency (16.7ms at 60fps).
    //
    // Default 1: this is an interactive remote display, and the extra frame is
    // felt directly when drawing with a stylus. At 60fps the synchronous
    // encode fits the 16.7ms budget with room to spare.
    //
    // Set STREAM_TABLET_ASYNC=2 for >90fps: a 120fps budget is 8.33ms, which a
    // blocking ~8ms encode cannot fit.
    int async_depth = 1;
    if (const char* e = std::getenv("STREAM_TABLET_ASYNC")) async_depth = atoi(e);
    if (async_depth < 1) async_depth = 1;
    av_opt_set_int((*codec_ctx)->priv_data, "async_depth", async_depth, 0);
    av_opt_set_int((*codec_ctx)->priv_data, "idr_interval", config.gop_size, 0);

    // Intel's fixed-function encode path (VDENC). Usually far faster than the
    // render-based path; on some parts it is the only one that supports AV1.
    if (const char* e = std::getenv("STREAM_TABLET_LOWPOWER")) {
        av_opt_set_int((*codec_ctx)->priv_data, "low_power", atoi(e), 0);
    }

    if (config.quality_mode == QualityMode::HIGH_QUALITY || config.quality_mode == QualityMode::AUTO) {
        // CQP mode - constant quality, variable bitrate
        // AUTO mode uses CQP for sharp text, with dynamic adjustment handled at runtime
        av_opt_set((*codec_ctx)->priv_data, "rc_mode", "CQP", 0);
        // Set quality level (lower = better quality)
        // AV1/HEVC: qp, H.264: qp
        av_opt_set_int((*codec_ctx)->priv_data, "qp", config.cqp, 0);
        // Also set global_quality for codecs that use it
        (*codec_ctx)->global_quality = config.cqp;
        // Higher bitrate cap for quality mode
        (*codec_ctx)->bit_rate = config.bitrate;
        (*codec_ctx)->rc_max_rate = config.bitrate * 2;
        (*codec_ctx)->rc_buffer_size = config.bitrate;
        // Quality preset - use fast for high FPS, medium otherwise
        // NOTE: "preset" is not a VAAPI encoder option (it belongs to QSV/NVENC).
        // av1_vaapi, hevc_vaapi and h264_vaapi all reject it, so the calls that
        // used to be here were silent no-ops. Speed on VAAPI comes from
        // async_depth and rc_mode instead. h264_vaapi alone accepts "quality".
        if (const char* e = std::getenv("STREAM_TABLET_PRESET")) {
            av_opt_set((*codec_ctx)->priv_data, "preset", e, 0);
        }
    } else {
        // CBR mode - constant bitrate
        (*codec_ctx)->bit_rate = config.bitrate;
        (*codec_ctx)->rc_max_rate = config.bitrate;
        // Smaller buffer = lower latency (1 frame worth of data)
        (*codec_ctx)->rc_buffer_size = config.bitrate / config.framerate;
        av_opt_set((*codec_ctx)->priv_data, "rc_mode", "CBR", 0);
        // "preset"/"tune" are not VAAPI options — see the note above.
    }

    // Frames context the encoder draws from. On the zero-copy path this is the
    // VPP filter's output pool — the encoder reads the surfaces scale_vaapi
    // wrote, so nothing is allocated or copied in between.
    if (dmabuf_active && *dmabuf_active) {
        AVBufferRef* vpp_out = dmabuf_pipeline->output_frames_ctx();
        if (!vpp_out) {
            avcodec_free_context(codec_ctx);
            av_buffer_unref(hw_device_ctx);
            dmabuf_pipeline->shutdown();
            return false;
        }
        *hw_frames_ctx = av_buffer_ref(vpp_out);
    } else {
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
    }

    (*codec_ctx)->hw_frames_ctx = av_buffer_ref(*hw_frames_ctx);

    // Try to open encoder - this is where we find out if the device supports it
    ret = avcodec_open2(*codec_ctx, codec, nullptr);
    if (ret < 0) {
        av_buffer_unref(hw_frames_ctx);
        avcodec_free_context(codec_ctx);
        av_buffer_unref(hw_device_ctx);
        if (dmabuf_pipeline) dmabuf_pipeline->shutdown();
        if (dmabuf_active) *dmabuf_active = false;
        return false;
    }

    return true;
}

bool VAAPIEncoder::init(const EncoderConfig& config) {
    m_config = config;

    // Suppress FFmpeg internal logging (only show errors in quiet mode)
    // Users can use -v to see our logs, but FFmpeg logs are too noisy
    av_log_set_level(AV_LOG_ERROR);

    // Get available render devices
    auto devices = get_render_devices();
    if (devices.empty()) {
        LOG_ERROR("No render devices found in /dev/dri/");
        return false;
    }

    LOG_INFO("Found %zu render device(s), probing for encoder support...", devices.size());

    // Define codec info
    struct CodecInfo {
        const char* encoder_name;
        const char* display_name;
        uint8_t codec_id;  // 0=AV1, 1=HEVC, 2=H264
    };

    // All available codecs
    const CodecInfo all_codecs[] = {
        {"av1_vaapi", "AV1", 0},
        {"hevc_vaapi", "HEVC", 1},
        {"h264_vaapi", "H.264", 2}
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
        for (const auto& device : devices) {
            LOG_INFO("Trying %s on %s...", codec.display_name, device.c_str());

            if (try_encoder_on_device(device.c_str(), codec.encoder_name, config,
                                       &m_impl->hw_device_ctx,
                                       &m_impl->hw_frames_ctx,
                                       &m_impl->codec_ctx,
                                       &m_dmabuf_pipeline,
                                       &m_dmabuf_active)) {
                LOG_INFO("Success! Using %s encoder on %s", codec.display_name, device.c_str());
                m_actual_codec = codec.codec_id;

                // Staging frames exist only for the CPU upload path. On the
                // zero-copy path the VPP output surface goes straight to the
                // encoder, and taking a surface from that pool here would just
                // shrink it.
                if (!m_dmabuf_active) {
                    m_impl->sw_frame = av_frame_alloc();
                    m_impl->sw_frame->format = AV_PIX_FMT_NV12;
                    m_impl->sw_frame->width = config.width;
                    m_impl->sw_frame->height = config.height;
                    av_frame_get_buffer(m_impl->sw_frame, 32);

                    m_impl->hw_frame = av_frame_alloc();
                    av_hwframe_get_buffer(m_impl->hw_frames_ctx, m_impl->hw_frame, 0);
                }

                m_impl->packet = av_packet_alloc();

                LOG_INFO("VAAPI encoder initialized: %dx%d @ %d fps, %d bps, codec=%s, input=%s",
                         config.width, config.height, config.framerate, config.bitrate,
                         codec.display_name,
                         m_dmabuf_active ? "DMA-BUF zero-copy" : "CPU upload");
                {
                    int64_t lp = -1, ad = -1;
                    av_opt_get_int(m_impl->codec_ctx->priv_data, "low_power", 0, &lp);
                    av_opt_get_int(m_impl->codec_ctx->priv_data, "async_depth", 0, &ad);
                    LOG_INFO("Encoder tuning: low_power=%lld async_depth=%lld",
                             (long long)lp, (long long)ad);
                }
                return true;
            }
        }
    }

    LOG_ERROR("No working VAAPI encoder found on any device");
    return false;
}

void VAAPIEncoder::shutdown() {
    m_impl.reset(new Impl());
    m_dmabuf_pipeline.shutdown();
    m_dmabuf_active = false;
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
    if (!m_impl->codec_ctx || !m_impl->sw_frame || !m_impl->hw_frame) {
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

    return encode_hw_frame(m_impl->hw_frame, timestamp_us, output);
}

bool VAAPIEncoder::encode_frame(const CapturedFrame& frame, EncodedFrame& output) {
    if (!m_dmabuf_active || !frame.is_dmabuf) {
        return EncoderBackend::encode_frame(frame, output);
    }
    if (!m_impl->codec_ctx) return false;

    // Import + GPU colour convert. No CPU touch of pixel data anywhere here.
    AVFrame* nv12 = static_cast<AVFrame*>(m_dmabuf_pipeline.process(frame));
    if (!nv12) return false;

    nv12->pts = static_cast<int64_t>(m_frame_count++);
    return encode_hw_frame(nv12, frame.timestamp_us, output);
}

bool VAAPIEncoder::encode_hw_frame(void* av_hw_frame, uint64_t timestamp_us,
                                   EncodedFrame& output) {
    AVFrame* hw = static_cast<AVFrame*>(av_hw_frame);

    // Force keyframe if requested
    if (m_force_keyframe.exchange(false, std::memory_order_relaxed)) {
        hw->pict_type = AV_PICTURE_TYPE_I;
        hw->flags |= AV_FRAME_FLAG_KEY;
        LOG_INFO("Forcing keyframe for frame %ld", m_frame_count);
    } else {
        hw->pict_type = AV_PICTURE_TYPE_NONE;
        hw->flags &= ~AV_FRAME_FLAG_KEY;
    }

    // Send frame to encoder
    auto t0 = std::chrono::high_resolution_clock::now();
    int ret = avcodec_send_frame(m_impl->codec_ctx, hw);
    if (ret < 0) {
        LOG_ERROR("Error sending frame to encoder");
        return false;
    }
    auto t1 = std::chrono::high_resolution_clock::now();

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

    auto t2 = std::chrono::high_resolution_clock::now();

    // Breakdown of the "encode" figure the server reports, so a slow VPP pass
    // is distinguishable from a slow codec.
    if (m_dmabuf_active) {
        static long acc_map = 0, acc_vpp = 0, acc_send = 0, acc_recv = 0;
        static int n = 0;
        static auto last_log = std::chrono::high_resolution_clock::now();
        acc_map  += m_dmabuf_pipeline.last_map_us();
        acc_vpp  += m_dmabuf_pipeline.last_vpp_us();
        acc_send += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        acc_recv += std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
        n++;
        if (std::chrono::duration_cast<std::chrono::seconds>(t2 - last_log).count() >= 5 && n > 0) {
            LOG_INFO("Encode breakdown (avg): import=%.2fms vpp=%.2fms send=%.2fms recv=%.2fms",
                     acc_map / 1000.0 / n, acc_vpp / 1000.0 / n,
                     acc_send / 1000.0 / n, acc_recv / 1000.0 / n);
            acc_map = acc_vpp = acc_send = acc_recv = 0;
            n = 0;
            last_log = t2;
        }
    }

    av_packet_unref(m_impl->packet);
    return true;
}

void VAAPIEncoder::set_bitrate(int bitrate) {
    m_config.bitrate = bitrate;
    // Would need to reinit encoder to change bitrate
}

}  // namespace stream_tablet
