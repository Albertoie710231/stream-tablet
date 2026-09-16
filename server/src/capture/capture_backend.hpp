#pragma once

#include <cstdint>

namespace stream_tablet {

// One plane of a DMA-BUF backed frame.
struct DmaBufPlane {
    int fd = -1;            // Not owned: valid until the next capture_frame()
    uint32_t offset = 0;
    uint32_t stride = 0;
};

// Captured frame data - used by all capture backends.
//
// Two mutually exclusive delivery modes:
//   CPU path  (is_dmabuf == false): `data` points at BGRA pixels in host memory.
//   GPU path  (is_dmabuf == true):  `planes` describe a DMA-BUF the encoder
//                                   imports directly; `data` is null and no
//                                   pixel ever touches the CPU.
struct CapturedFrame {
    uint8_t* data = nullptr;      // Pixel data (BGRA format), CPU path only
    int width = 0;                // Frame width in pixels
    int height = 0;               // Frame height in pixels
    int stride = 0;               // Bytes per row (usually width * 4)
    uint64_t timestamp_us = 0;    // Timestamp in microseconds

    // GPU path. The fds stay valid until the next capture_frame() call on the
    // same backend — the backend holds the producer's buffer until then.
    bool is_dmabuf = false;
    DmaBufPlane planes[4];
    int n_planes = 0;
    uint64_t modifier = 0;        // DRM format modifier
    uint32_t drm_format = 0;      // DRM fourcc
};

// Abstract capture backend interface
class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;

    // Initialize the capture backend
    // Returns true on success
    virtual bool init(const char* display_name = nullptr) = 0;

    // Shutdown and cleanup resources
    virtual void shutdown() = 0;

    // Capture a frame (blocking)
    // Returns true if a new frame was captured
    virtual bool capture_frame(CapturedFrame& frame) = 0;

    // Get screen dimensions (only valid after init)
    virtual int get_width() const = 0;
    virtual int get_height() const = 0;

    // Check if initialized successfully
    virtual bool is_initialized() const = 0;

    // Get backend name for logging
    virtual const char* get_name() const = 0;

    // Set target framerate (hint for backends that support it)
    virtual void set_framerate(int fps) { (void)fps; }

    // True once the backend has negotiated a zero-copy DMA-BUF stream. The
    // encoder queries this after init to decide whether to build a GPU import
    // pipeline instead of its software colour-conversion path.
    virtual bool is_dmabuf_capture() const { return false; }
    virtual uint32_t get_drm_format() const { return 0; }
    virtual uint64_t get_drm_modifier() const { return 0; }

    // Renegotiate the stream without DMA-BUF. Called when the encoder turns out
    // not to be able to import what we negotiated. Returns true if the backend
    // is now delivering CPU buffers.
    virtual bool disable_dmabuf() { return true; }

protected:
    CaptureBackend() = default;

    // Non-copyable
    CaptureBackend(const CaptureBackend&) = delete;
    CaptureBackend& operator=(const CaptureBackend&) = delete;
};

}  // namespace stream_tablet
