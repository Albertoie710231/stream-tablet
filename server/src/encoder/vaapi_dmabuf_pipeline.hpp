#pragma once

#include <cstdint>
#include "../capture/capture_backend.hpp"

struct AVBufferRef;
struct AVFrame;

namespace stream_tablet {

// Zero-copy bridge from a PipeWire DMA-BUF to a VAAPI NV12 surface.
//
//   DMA-BUF fd  --(DRM_PRIME)-->  VAAPI BGRx surface  --(VPP)-->  VAAPI NV12
//
// Nothing crosses the PCIe bus and no pixel touches the CPU: the import is an
// fd hand-off and the colour conversion runs on the GPU's video-processing
// block via FFmpeg's scale_vaapi filter. Replaces convert_frame(),
// convert_bgra_to_nv12_fast() and av_hwframe_transfer_data() on this path.
class VaapiDmaBufPipeline {
public:
    VaapiDmaBufPipeline();
    ~VaapiDmaBufPipeline();

    VaapiDmaBufPipeline(const VaapiDmaBufPipeline&) = delete;
    VaapiDmaBufPipeline& operator=(const VaapiDmaBufPipeline&) = delete;

    // `va_device` is the encoder's existing AV_HWDEVICE_TYPE_VAAPI context.
    bool init(AVBufferRef* va_device, const char* drm_device_path,
              int width, int height,
              uint32_t drm_format, uint64_t drm_modifier, int framerate);

    // Imports one captured DMA-BUF and returns a VAAPI/NV12 frame owned by the
    // pipeline, valid until the next call. Returns nullptr on failure.
    AVFrame* process(const CapturedFrame& frame);

    // The frames context the encoder must be opened against — it is the VPP
    // output pool, not a pool the encoder allocates itself.
    AVBufferRef* output_frames_ctx() const;

    void shutdown();

    // Per-frame cost of the last process() call, in microseconds. Split so we
    // can tell an expensive import from an expensive colour-conversion pass.
    long last_map_us() const;
    long last_vpp_us() const;

private:
    struct Impl;
    Impl* m_impl;
};

}  // namespace stream_tablet
