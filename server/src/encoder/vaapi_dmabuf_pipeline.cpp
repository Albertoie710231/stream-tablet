#include "vaapi_dmabuf_pipeline.hpp"
#include "../util/logger.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <libdrm/drm_fourcc.h>
#include <chrono>
#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace stream_tablet {

namespace {

// DRM fourcc -> the AVPixelFormat describing the same byte order.
AVPixelFormat drm_fourcc_to_av_pixfmt(uint32_t fourcc) {
    switch (fourcc) {
        case DRM_FORMAT_XRGB8888: return AV_PIX_FMT_BGR0;
        case DRM_FORMAT_ARGB8888: return AV_PIX_FMT_BGRA;
        case DRM_FORMAT_XBGR8888: return AV_PIX_FMT_RGB0;
        case DRM_FORMAT_ABGR8888: return AV_PIX_FMT_RGBA;
        default:                  return AV_PIX_FMT_NONE;
    }
}

// The AVDRMFrameDescriptor we hand to FFmpeg is a member of Impl, not heap
// memory, so the AVBufferRef wrapping it must not free anything.
void drm_desc_no_free(void*, uint8_t*) {}

}  // namespace

struct VaapiDmaBufPipeline::Impl {
    AVBufferRef* drm_device = nullptr;   // AV_HWDEVICE_TYPE_DRM
    AVBufferRef* drm_frames = nullptr;   // DRM_PRIME frames pool
    AVBufferRef* va_frames_src = nullptr;// VAAPI view of drm_frames (BGRx)

    AVFilterGraph* graph = nullptr;
    AVFilterContext* buffersrc = nullptr;
    AVFilterContext* buffersink = nullptr;

    AVFrame* drm_frame = nullptr;        // wraps the incoming fd
    AVFrame* mapped_frame = nullptr;     // VAAPI BGRx
    AVFrame* out_frame = nullptr;        // VAAPI NV12 (VPP output)

    AVDRMFrameDescriptor drm_desc{};
    int64_t pts = 0;
    long map_us = 0;
    long vpp_us = 0;

    ~Impl() {
        if (out_frame) av_frame_free(&out_frame);
        if (mapped_frame) av_frame_free(&mapped_frame);
        if (drm_frame) av_frame_free(&drm_frame);
        if (graph) avfilter_graph_free(&graph);
        if (va_frames_src) av_buffer_unref(&va_frames_src);
        if (drm_frames) av_buffer_unref(&drm_frames);
        if (drm_device) av_buffer_unref(&drm_device);
    }
};

VaapiDmaBufPipeline::VaapiDmaBufPipeline() : m_impl(new Impl()) {}

VaapiDmaBufPipeline::~VaapiDmaBufPipeline() {
    delete m_impl;
    m_impl = nullptr;
}

void VaapiDmaBufPipeline::shutdown() {
    delete m_impl;
    m_impl = new Impl();
}

long VaapiDmaBufPipeline::last_map_us() const { return m_impl->map_us; }
long VaapiDmaBufPipeline::last_vpp_us() const { return m_impl->vpp_us; }

AVBufferRef* VaapiDmaBufPipeline::output_frames_ctx() const {
    if (!m_impl->buffersink) return nullptr;
    return av_buffersink_get_hw_frames_ctx(m_impl->buffersink);
}

bool VaapiDmaBufPipeline::init(AVBufferRef* va_device, const char* drm_device_path,
                               int width, int height,
                               uint32_t drm_format, uint64_t drm_modifier,
                               int framerate) {
    auto* I = m_impl;

    AVPixelFormat sw_fmt = drm_fourcc_to_av_pixfmt(drm_format);
    if (sw_fmt == AV_PIX_FMT_NONE) {
        LOG_WARN("dmabuf: unsupported DRM fourcc 0x%08x, falling back to CPU path", drm_format);
        return false;
    }

    // 1) A DRM device context for the DRM_PRIME frames pool. It must be related
    //    to the VAAPI device or the later frames-context derivation fails with
    //    EINVAL — this is the same "vaapi=va@drm" relationship ffmpeg sets up
    //    for its kmsgrab zero-copy path, just derived in the other direction.
    int ret = av_hwdevice_ctx_create_derived(&I->drm_device, AV_HWDEVICE_TYPE_DRM,
                                             va_device, 0);
    if (ret < 0) {
        LOG_WARN("dmabuf: deriving DRM device from VAAPI failed (%d), "
                 "falling back to opening %s directly", ret, drm_device_path);
        ret = av_hwdevice_ctx_create(&I->drm_device, AV_HWDEVICE_TYPE_DRM,
                                     drm_device_path, nullptr, 0);
        if (ret < 0) {
            LOG_WARN("dmabuf: av_hwdevice_ctx_create(DRM) failed (%d)", ret);
            return false;
        }
    }

    // 2) A DRM_PRIME frames pool describing the buffers KWin hands us.
    I->drm_frames = av_hwframe_ctx_alloc(I->drm_device);
    if (!I->drm_frames) return false;
    auto* dfc = reinterpret_cast<AVHWFramesContext*>(I->drm_frames->data);
    dfc->format = AV_PIX_FMT_DRM_PRIME;
    dfc->sw_format = sw_fmt;
    dfc->width = width;
    dfc->height = height;
    dfc->initial_pool_size = 0;  // we supply the buffers, don't allocate any
    ret = av_hwframe_ctx_init(I->drm_frames);
    if (ret < 0) {
        LOG_WARN("dmabuf: DRM frames ctx init failed (%d)", ret);
        return false;
    }

    // 3) A VAAPI frames context derived from the DRM one. Deriving (rather than
    //    allocating) is what makes av_hwframe_map a pointer hand-off instead of
    //    a copy.
    ret = av_hwframe_ctx_create_derived(&I->va_frames_src, AV_PIX_FMT_VAAPI,
                                        va_device, I->drm_frames,
                                        AV_HWFRAME_MAP_DIRECT);
    if (ret < 0) {
        LOG_WARN("dmabuf: av_hwframe_ctx_create_derived failed (%d) — "
                 "driver may not import modifier 0x%llx", ret,
                 static_cast<unsigned long long>(drm_modifier));
        return false;
    }

    // 4) Filter graph: buffersrc -> scale_vaapi(format=nv12) -> buffersink.
    //    scale_vaapi runs on the GPU's video-processing block.
    I->graph = avfilter_graph_alloc();
    if (!I->graph) return false;

    // A hardware buffersrc must be built in three steps: allocate uninitialised,
    // attach the hw_frames_ctx via AVBufferSrcParameters, then init. Passing an
    // args string to avfilter_graph_create_filter initialises it too early and
    // the hardware frames context is rejected with EINVAL.
    I->buffersrc = avfilter_graph_alloc_filter(I->graph, avfilter_get_by_name("buffer"), "in");
    if (!I->buffersrc) {
        LOG_WARN("dmabuf: buffersrc alloc failed");
        return false;
    }

    char size_str[64];
    snprintf(size_str, sizeof(size_str), "%dx%d", width, height);
    av_opt_set(I->buffersrc, "video_size", size_str, AV_OPT_SEARCH_CHILDREN);
    av_opt_set(I->buffersrc, "pix_fmt", "vaapi", AV_OPT_SEARCH_CHILDREN);
    av_opt_set(I->buffersrc, "time_base", "1/1000000", AV_OPT_SEARCH_CHILDREN);
    av_opt_set(I->buffersrc, "pixel_aspect", "1/1", AV_OPT_SEARCH_CHILDREN);
    {
        char fr[32];
        snprintf(fr, sizeof(fr), "%d/1", framerate > 0 ? framerate : 60);
        av_opt_set(I->buffersrc, "frame_rate", fr, AV_OPT_SEARCH_CHILDREN);
    }

    AVBufferSrcParameters* par = av_buffersrc_parameters_alloc();
    if (!par) return false;
    par->format = AV_PIX_FMT_VAAPI;
    par->width = width;
    par->height = height;
    par->time_base = AVRational{1, 1000000};
    par->hw_frames_ctx = I->va_frames_src;
    ret = av_buffersrc_parameters_set(I->buffersrc, par);
    av_free(par);
    if (ret < 0) {
        LOG_WARN("dmabuf: av_buffersrc_parameters_set failed (%d)", ret);
        return false;
    }

    ret = avfilter_init_str(I->buffersrc, nullptr);
    if (ret < 0) {
        LOG_WARN("dmabuf: buffersrc init failed (%d)", ret);
        return false;
    }

    I->buffersink = avfilter_graph_alloc_filter(I->graph, avfilter_get_by_name("buffersink"), "out");
    if (!I->buffersink) {
        LOG_WARN("dmabuf: buffersink alloc failed");
        return false;
    }
    ret = avfilter_init_str(I->buffersink, nullptr);
    if (ret < 0) {
        LOG_WARN("dmabuf: buffersink init failed (%d)", ret);
        return false;
    }

    AVFilterContext* scale = avfilter_graph_alloc_filter(
        I->graph, avfilter_get_by_name("scale_vaapi"), "vpp");
    if (!scale) {
        LOG_WARN("dmabuf: scale_vaapi unavailable");
        return false;
    }
    av_opt_set(scale, "format", "nv12", AV_OPT_SEARCH_CHILDREN);
    ret = avfilter_init_str(scale, nullptr);
    if (ret < 0) {
        LOG_WARN("dmabuf: scale_vaapi init failed (%d)", ret);
        return false;
    }

    if ((ret = avfilter_link(I->buffersrc, 0, scale, 0)) < 0 ||
        (ret = avfilter_link(scale, 0, I->buffersink, 0)) < 0) {
        LOG_WARN("dmabuf: filter link failed (%d)", ret);
        return false;
    }

    ret = avfilter_graph_config(I->graph, nullptr);
    if (ret < 0) {
        LOG_WARN("dmabuf: filter graph config failed (%d)", ret);
        return false;
    }

    I->drm_frame = av_frame_alloc();
    I->mapped_frame = av_frame_alloc();
    I->out_frame = av_frame_alloc();
    if (!I->drm_frame || !I->mapped_frame || !I->out_frame) return false;

    LOG_INFO("dmabuf: GPU import pipeline ready — %s %dx%d, modifier 0x%llx -> VAAPI NV12",
             av_get_pix_fmt_name(sw_fmt), width, height,
             static_cast<unsigned long long>(drm_modifier));
    return true;
}

AVFrame* VaapiDmaBufPipeline::process(const CapturedFrame& frame) {
    auto* I = m_impl;
    if (!I->graph || !frame.is_dmabuf || frame.n_planes <= 0) return nullptr;

    // Describe the incoming buffer. Single layer, one object per plane — which
    // for the RGB formats KWin exports is always exactly one of each.
    AVDRMFrameDescriptor& d = I->drm_desc;
    memset(&d, 0, sizeof(d));
    d.nb_objects = 1;
    d.objects[0].fd = frame.planes[0].fd;
    // The iHD driver rejects a zero-sized object, so ask the kernel for the real
    // dmabuf size (dmabuf fds support SEEK_END) and fall back to the
    // conservative layout estimate if that is not available.
    off_t sz = lseek(frame.planes[0].fd, 0, SEEK_END);
    if (sz <= 0) {
        sz = static_cast<off_t>(frame.planes[0].offset) +
             static_cast<off_t>(frame.planes[0].stride) * frame.height;
    }
    d.objects[0].size = static_cast<size_t>(sz);
    d.objects[0].format_modifier = frame.modifier;

    d.nb_layers = 1;
    d.layers[0].format = frame.drm_format;
    d.layers[0].nb_planes = frame.n_planes;
    for (int i = 0; i < frame.n_planes && i < 4; i++) {
        d.layers[0].planes[i].object_index = 0;
        d.layers[0].planes[i].offset = frame.planes[i].offset;
        d.layers[0].planes[i].pitch = frame.planes[i].stride;
    }

    av_frame_unref(I->drm_frame);
    I->drm_frame->format = AV_PIX_FMT_DRM_PRIME;
    I->drm_frame->width = frame.width;
    I->drm_frame->height = frame.height;
    I->drm_frame->data[0] = reinterpret_cast<uint8_t*>(&d);
    // av_hwframe_map refs the source frame's buf[0] to keep the mapping alive.
    // A frame with no AVBufferRef backing is rejected with EINVAL, so wrap the
    // descriptor in a non-owning buffer.
    I->drm_frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(&d), sizeof(d),
                                            drm_desc_no_free, nullptr,
                                            AV_BUFFER_FLAG_READONLY);
    if (!I->drm_frame->buf[0]) return nullptr;
    I->drm_frame->hw_frames_ctx = av_buffer_ref(I->drm_frames);
    if (!I->drm_frame->hw_frames_ctx) return nullptr;

    auto t_map0 = std::chrono::high_resolution_clock::now();

    // Import: binds the fd to a VA surface. No pixel copy.
    av_frame_unref(I->mapped_frame);
    I->mapped_frame->format = AV_PIX_FMT_VAAPI;
    I->mapped_frame->hw_frames_ctx = av_buffer_ref(I->va_frames_src);
    int ret = av_hwframe_map(I->mapped_frame, I->drm_frame,
                             AV_HWFRAME_MAP_DIRECT | AV_HWFRAME_MAP_READ);
    if (ret < 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, err, sizeof(err));
            LOG_ERROR("dmabuf: av_hwframe_map failed (%d: %s) fd=%d size=%zu "
                      "stride=%u modifier=0x%llx",
                      ret, err, d.objects[0].fd, d.objects[0].size,
                      frame.planes[0].stride,
                      static_cast<unsigned long long>(frame.modifier));
        }
        return nullptr;
    }

    I->mapped_frame->pts = static_cast<int64_t>(frame.timestamp_us);

    auto t_map1 = std::chrono::high_resolution_clock::now();
    I->map_us = std::chrono::duration_cast<std::chrono::microseconds>(t_map1 - t_map0).count();

    ret = av_buffersrc_add_frame_flags(I->buffersrc, I->mapped_frame,
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) {
        LOG_ERROR("dmabuf: buffersrc add_frame failed (%d)", ret);
        return nullptr;
    }

    av_frame_unref(I->out_frame);
    ret = av_buffersink_get_frame(I->buffersink, I->out_frame);
    if (ret < 0) {
        if (ret != AVERROR(EAGAIN)) {
            LOG_ERROR("dmabuf: buffersink get_frame failed (%d)", ret);
        }
        return nullptr;
    }

    auto t_vpp1 = std::chrono::high_resolution_clock::now();
    I->vpp_us = std::chrono::duration_cast<std::chrono::microseconds>(t_vpp1 - t_map1).count();

    return I->out_frame;
}

}  // namespace stream_tablet
