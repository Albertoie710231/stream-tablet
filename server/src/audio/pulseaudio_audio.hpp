#pragma once

#include "audio_backend.hpp"
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

struct pa_simple;

namespace stream_tablet {

class PulseAudioAudio : public AudioBackend {
public:
    PulseAudioAudio();
    ~PulseAudioAudio() override;

    bool init(const AudioConfig& config) override;
    void shutdown() override;

    bool start(AudioCallback callback) override;
    void stop() override;

    bool is_initialized() const override { return m_initialized; }
    bool is_capturing() const override { return m_capturing; }
    const char* get_name() const override { return "PulseAudio"; }

    int get_sample_rate() const override { return m_sample_rate; }
    int get_channels() const override { return m_channels; }

private:
    void capture_thread();
    std::string find_monitor_source();

    AudioConfig m_config;
    AudioCallback m_callback;

    struct pa_simple* m_pa = nullptr;

    std::thread m_thread;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_capturing{false};
    std::atomic<bool> m_running{false};

    int m_sample_rate = 48000;
    int m_channels = 2;
    int m_frame_size = 480;  // samples per channel (10ms at 48kHz)

    std::vector<float> m_audio_buffer;
    std::mutex m_callback_mutex;
};

}  // namespace stream_tablet
