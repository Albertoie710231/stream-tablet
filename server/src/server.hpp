#pragma once

#include <memory>
#include <atomic>
#include "stream_tablet/config.hpp"
#include "capture/x11_capture.hpp"
#include "encoder/vaapi_encoder.hpp"
#include "network/control_server.hpp"
#include "network/video_sender.hpp"
#include "network/input_receiver.hpp"
#include "input/uinput_backend.hpp"
#include "input/coord_transform.hpp"

namespace stream_tablet {

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

private:
    void capture_and_encode_loop();
    void handle_input(const InputEvent& event);

    ServerConfig m_config;

    std::unique_ptr<X11Capture> m_capture;
    std::unique_ptr<VAAPIEncoder> m_encoder;
    std::unique_ptr<ControlServer> m_control;
    std::unique_ptr<VideoSender> m_video_sender;
    std::unique_ptr<InputReceiver> m_input_receiver;
    std::unique_ptr<UInputBackend> m_uinput;

    CoordTransform m_coord_transform;

    std::atomic<bool> m_running{false};
    uint32_t m_frame_count = 0;
};

}  // namespace stream_tablet
