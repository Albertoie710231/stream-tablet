#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <vector>
#include <memory>

namespace stream_tablet {

struct AudioConfig {
    int sample_rate = 48000;
    int channels = 2;
    int frame_size_ms = 10;  // milliseconds per frame
    std::string device;      // empty = default monitor, or specific sink/app name
};

struct AudioFrame {
    const float* samples;    // Interleaved float samples [-1.0, 1.0]
    int num_samples;         // Per channel
    int channels;
    uint64_t timestamp_us;   // Timestamp in microseconds
};

using AudioCallback = std::function<void(const AudioFrame&)>;

class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    // Initialize the audio backend with the given configuration
    virtual bool init(const AudioConfig& config) = 0;

    // Clean up resources
    virtual void shutdown() = 0;

    // Start capturing audio, calling the callback for each frame
    virtual bool start(AudioCallback callback) = 0;

    // Stop capturing audio
    virtual void stop() = 0;

    // Check if the backend is initialized
    virtual bool is_initialized() const = 0;

    // Check if currently capturing
    virtual bool is_capturing() const = 0;

    // Get the backend name for logging
    virtual const char* get_name() const = 0;

    // Get actual sample rate (may differ from requested)
    virtual int get_sample_rate() const = 0;

    // Get actual channel count
    virtual int get_channels() const = 0;

protected:
    AudioBackend() = default;
    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;
};

// Audio backend type for selection
enum class AudioBackendType {
    AUTO,       // Try PipeWire first, fall back to PulseAudio
    PIPEWIRE,
    PULSEAUDIO
};

// Factory function to create an audio backend
// Returns nullptr if no suitable backend is available
std::unique_ptr<AudioBackend> create_audio_backend(AudioBackendType type = AudioBackendType::AUTO);

}  // namespace stream_tablet
