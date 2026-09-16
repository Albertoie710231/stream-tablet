#pragma once

#include <memory>
#include <atomic>
#include <mutex>
#include "stream_tablet/config.hpp"
#include "capture/capture_backend.hpp"
#include "encoder/encoder_backend.hpp"
#include "network/control_server.hpp"
#include "network/mdns_publisher.hpp"
#include "network/video_sender.hpp"
#include "network/input_receiver.hpp"
#include "input/uinput_backend.hpp"
#include "input/coord_transform.hpp"

#ifdef HAVE_OPUS
#include "audio/audio_backend.hpp"
#include "audio/audio_router.hpp"
#include "audio/opus_encoder.hpp"
#include "network/audio_sender.hpp"
#endif

namespace stream_tablet {

// Capture backend type
enum class CaptureBackendType {
    AUTO,       // Auto-detect based on environment
    X11,        // Force X11 capture
    PIPEWIRE    // Force PipeWire capture
};

class Server {
public:
    Server();
    ~Server();

    // Initialize with configuration
    bool init(const ServerConfig& config);

    // Run server (blocking)
    void run();

    // Stop server
    void stop();

    // Set preferred capture backend (call before init)
    void set_capture_backend(CaptureBackendType type) { m_backend_type = type; }

private:
    bool create_capture_backend(const char* display);
    bool init_encoder_from_client(const ClientInfo& client);
    bool capture_and_encode_loop();  // returns true if a frame was captured+encoded
    // Drops capture back to CPU buffers and rebuilds the encoder. Used both at
    // init and when the zero-copy path fails at run time.
    bool fall_back_to_cpu_capture();
    void handle_input(const InputEvent& event);

#ifdef HAVE_OPUS
    bool init_audio();
    void on_audio_frame(const AudioFrame& frame);
#endif

    ServerConfig m_config;
    CaptureBackendType m_backend_type = CaptureBackendType::AUTO;

    std::unique_ptr<CaptureBackend> m_capture;
    std::unique_ptr<EncoderBackend> m_encoder;
    std::unique_ptr<ControlServer> m_control;
    MdnsPublisher m_mdns;
    std::unique_ptr<VideoSender> m_video_sender;
    std::unique_ptr<InputReceiver> m_input_receiver;
    std::unique_ptr<UInputBackend> m_uinput;

    CoordTransform m_coord_transform;

#ifdef HAVE_OPUS
    // Audio components
    std::unique_ptr<AudioBackend> m_audio_capture;
    std::unique_ptr<OpusEncoder> m_opus_encoder;
    std::unique_ptr<AudioSender> m_audio_sender;
    AudioRouter m_audio_router;
    bool m_audio_initialized = false;
    uint32_t m_audio_sequence = 0;
    std::mutex m_audio_mutex;
#endif

    std::atomic<bool> m_running{false};
    uint32_t m_frame_count = 0;

    // Kept so the encoder can be rebuilt without redoing the client handshake.
    EncoderConfig m_encoder_config;
    uint32_t m_dmabuf_encode_failures = 0;
};

}  // namespace stream_tablet
