#pragma once

#include "audio_backend.hpp"
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

struct pw_main_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_hook;

namespace stream_tablet {

class PipeWireAudio : public AudioBackend {
public:
    PipeWireAudio();
    ~PipeWireAudio() override;

    bool init(const AudioConfig& config) override;
    void shutdown() override;

    bool start(AudioCallback callback) override;
    void stop() override;

    bool is_initialized() const override { return m_initialized; }
    bool is_capturing() const override { return m_capturing; }
    const char* get_name() const override { return "PipeWire"; }

    int get_sample_rate() const override { return m_sample_rate; }
    int get_channels() const override { return m_channels; }

    // PipeWire callbacks (public for C callback access)
    void on_stream_process();
    void on_stream_param_changed(uint32_t id, const void* param);
    void on_stream_state_changed(int old_state, int state, const char* error);

private:
    bool init_pipewire();
    bool connect_to_monitor();
    void cleanup_pipewire();
    void stream_thread();

    AudioConfig m_config;
    AudioCallback m_callback;

    struct pw_main_loop* m_pw_loop = nullptr;
    struct pw_context* m_pw_context = nullptr;
    struct pw_core* m_pw_core = nullptr;
    struct pw_stream* m_pw_stream = nullptr;
    struct spa_hook* m_stream_listener = nullptr;

    std::thread m_thread;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_capturing{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stream_ready{false};

    int m_sample_rate = 48000;
    int m_channels = 2;
    int m_frame_size = 480;  // samples per channel (10ms at 48kHz)

    std::vector<float> m_audio_buffer;
    std::mutex m_callback_mutex;
};

}  // namespace stream_tablet
