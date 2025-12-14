#pragma once

#include <cstdint>

namespace stream_tablet {

// Captured frame data - used by all capture backends
struct CapturedFrame {
    uint8_t* data = nullptr;      // Pixel data (BGRA format)
    int width = 0;                // Frame width in pixels
    int height = 0;               // Frame height in pixels
    int stride = 0;               // Bytes per row (usually width * 4)
    uint64_t timestamp_us = 0;    // Timestamp in microseconds
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

protected:
    CaptureBackend() = default;

    // Non-copyable
    CaptureBackend(const CaptureBackend&) = delete;
    CaptureBackend& operator=(const CaptureBackend&) = delete;
};

}  // namespace stream_tablet
