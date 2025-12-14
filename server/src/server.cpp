#include "server.hpp"
#include "util/logger.hpp"
#include <chrono>
#include <thread>

namespace stream_tablet {

Server::Server() = default;

Server::~Server() {
    stop();
}

bool Server::init(const ServerConfig& config) {
    m_config = config;

    // Initialize X11 capture
    m_capture = std::make_unique<X11Capture>();
    if (!m_capture->init(config.display.c_str())) {
        LOG_ERROR("Failed to initialize X11 capture");
        return false;
    }

    // Initialize VA-API encoder
    EncoderConfig enc_config;
    enc_config.width = m_capture->get_width();
    enc_config.height = m_capture->get_height();
    enc_config.framerate = config.capture_fps;
    enc_config.bitrate = config.bitrate;
    enc_config.gop_size = config.gop_size;
    enc_config.low_latency = true;

    m_encoder = std::make_unique<VAAPIEncoder>();
    if (!m_encoder->init(enc_config)) {
        LOG_ERROR("Failed to initialize VA-API encoder");
        return false;
    }

    // Initialize control server
    m_control = std::make_unique<ControlServer>();
    if (!m_control->init_plain(config.control_port)) {
        LOG_ERROR("Failed to initialize control server");
        return false;
    }

    // Initialize video sender
    m_video_sender = std::make_unique<VideoSender>();
    if (!m_video_sender->init(config.video_port)) {
        LOG_ERROR("Failed to initialize video sender");
        return false;
    }

    // Initialize input receiver
    m_input_receiver = std::make_unique<InputReceiver>();
    if (!m_input_receiver->init(config.input_port)) {
        LOG_ERROR("Failed to initialize input receiver");
        return false;
    }

    // Initialize uinput
    m_uinput = std::make_unique<UInputBackend>();
    if (!m_uinput->init(m_capture->get_width(), m_capture->get_height())) {
        LOG_WARN("Failed to initialize uinput (stylus input may not work)");
        // Continue without uinput - it's not fatal
    }

    // Set input callback
    m_input_receiver->set_callback([this](const InputEvent& event) {
        handle_input(event);
    });

    // Set keyframe callback
    m_control->set_keyframe_callback([this]() {
        LOG_INFO("Keyframe requested by client");
        m_encoder->request_keyframe();
    });

    LOG_INFO("Server initialized: %dx%d @ %d fps",
             m_capture->get_width(), m_capture->get_height(), config.capture_fps);
    return true;
}

void Server::run() {
    m_running = true;

    while (m_running) {
        LOG_INFO("Waiting for client connection...");

        // Wait for client to connect
        ClientInfo client_info;
        if (!m_control->accept_client(client_info)) {
            if (!m_running) break;
            LOG_ERROR("Failed to accept client");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // Send configuration to client
        m_control->send_config(m_capture->get_width(), m_capture->get_height(),
                               m_config.video_port, m_config.input_port);

        // Set video destination
        m_video_sender->set_client(client_info.host, client_info.video_port);

        // Initialize coordinate transform
        m_coord_transform.init(m_capture->get_width(), m_capture->get_height(),
                               client_info.width, client_info.height,
                               CoordTransform::Mode::LETTERBOX, false);

        LOG_INFO("Client connected, starting stream...");

        // Reset frame count for new session
        m_frame_count = 0;
        m_encoder->request_keyframe();  // Start with a keyframe

        // Calculate frame interval
        auto frame_interval = std::chrono::microseconds(1000000 / m_config.capture_fps);
        auto next_frame = std::chrono::high_resolution_clock::now();

        // Stream loop - runs until client disconnects
        while (m_running && m_control->is_client_connected()) {
            auto now = std::chrono::high_resolution_clock::now();

            // Process control messages
            m_control->process();

            // Process input events with high priority (no sleep between)
            m_input_receiver->process();

            // Check if it's time for next frame
            if (now >= next_frame) {
                capture_and_encode_loop();
                next_frame += frame_interval;

                // If we're behind, skip frames
                if (next_frame < now) {
                    next_frame = now + frame_interval;
                }
            }

            // Calculate time until next frame and sleep smartly
            auto time_to_next = std::chrono::duration_cast<std::chrono::microseconds>(
                next_frame - std::chrono::high_resolution_clock::now());

            if (time_to_next.count() > 1000) {
                // Sleep for half the remaining time to allow input processing
                std::this_thread::sleep_for(time_to_next / 2);
            } else if (time_to_next.count() > 100) {
                // Very short sleep just to yield CPU
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            // If less than 100us, don't sleep - busy wait for accuracy
        }

        if (m_running) {
            LOG_INFO("Client disconnected, waiting for new connection...");
            // Release all pressed buttons/tools before resetting
            if (m_uinput && m_uinput->is_initialized()) {
                m_uinput->reset_all();
            }
            m_control->reset();
            m_input_receiver->reset();
        }
    }

    LOG_INFO("Server stopped");
}

void Server::capture_and_encode_loop() {
    // Capture frame
    CapturedFrame frame;
    if (!m_capture->capture_frame(frame)) {
        return;
    }

    // Encode frame
    EncodedFrame encoded;
    if (!m_encoder->encode(frame.data, frame.width, frame.height, frame.stride,
                           frame.timestamp_us, encoded)) {
        return;  // Encoder not ready yet or error
    }

    // Send to client
    bool sent = m_video_sender->send_frame(encoded.data.data(), encoded.data.size(),
                               m_frame_count, encoded.is_keyframe, encoded.timestamp_us);

    if (m_frame_count % 60 == 0 || encoded.is_keyframe) {
        LOG_DEBUG("Frame %d: %zu bytes, keyframe=%d, sent=%d",
                  m_frame_count, encoded.data.size(), encoded.is_keyframe, sent);
    }
    m_frame_count++;
}

void Server::handle_input(const InputEvent& event) {
    if (!m_uinput || !m_uinput->is_initialized()) {
        return;
    }

    // Transform coordinates
    int screen_x, screen_y;
    m_coord_transform.transform(event.x, event.y, screen_x, screen_y);

    switch (event.type) {
        case InputEventType::STYLUS_DOWN:
        case InputEventType::STYLUS_MOVE:
        case InputEventType::STYLUS_HOVER: {
            bool tip_down = (event.type != InputEventType::STYLUS_HOVER);
            bool button1 = (event.buttons & 0x02) != 0;  // Secondary button
            bool button2 = (event.buttons & 0x04) != 0;  // Tertiary button
            bool eraser = (event.buttons & 0x20) != 0;   // Eraser mode

            m_uinput->send_stylus(screen_x, screen_y, event.pressure,
                                   event.tilt_x, event.tilt_y,
                                   tip_down, button1, button2, eraser);
            m_uinput->sync();
            break;
        }

        case InputEventType::STYLUS_UP: {
            // Pass in_range=false to release BTN_TOOL_PEN
            m_uinput->send_stylus(screen_x, screen_y, 0.0f,
                                   event.tilt_x, event.tilt_y,
                                   false, false, false, false, false);
            m_uinput->sync();
            break;
        }

        case InputEventType::TOUCH_DOWN:
        case InputEventType::TOUCH_MOVE:
            m_uinput->send_touch(screen_x, screen_y, event.pointer_id, true, event.pressure);
            m_uinput->sync();
            break;

        case InputEventType::TOUCH_UP:
            m_uinput->send_touch(screen_x, screen_y, event.pointer_id, false, 0.0f);
            m_uinput->sync();
            break;

        default:
            break;
    }
}

void Server::stop() {
    m_running = false;
}

}  // namespace stream_tablet
