#include "server.hpp"
#include "util/logger.hpp"
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstdio>

#ifdef HAVE_X11
#include "capture/x11_capture.hpp"
#endif

#ifdef HAVE_PIPEWIRE
#include "capture/pipewire_capture.hpp"
#endif

#include "encoder/encoder_factory.hpp"
#include "display/kwin_output.hpp"

namespace stream_tablet {

Server::Server() = default;

Server::~Server() {
    stop();
}

bool Server::create_capture_backend(const char* display) {
    CaptureBackendType backend = m_backend_type;

    // Auto-detect if needed
    if (backend == CaptureBackendType::AUTO) {
        const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
        const char* x11_display = std::getenv("DISPLAY");

        if (wayland_display && wayland_display[0] != '\0') {
#ifdef HAVE_PIPEWIRE
            LOG_INFO("Detected Wayland session, using PipeWire capture");
            backend = CaptureBackendType::PIPEWIRE;
#elif defined(HAVE_X11)
            LOG_WARN("Wayland detected but PipeWire not available, falling back to X11");
            backend = CaptureBackendType::X11;
#else
            LOG_ERROR("Wayland detected but no capture backend available");
            return false;
#endif
        } else if (x11_display && x11_display[0] != '\0') {
#ifdef HAVE_X11
            LOG_INFO("Detected X11 session, using X11 capture");
            backend = CaptureBackendType::X11;
#elif defined(HAVE_PIPEWIRE)
            LOG_WARN("X11 detected but X11 capture not available, trying PipeWire");
            backend = CaptureBackendType::PIPEWIRE;
#else
            LOG_ERROR("X11 detected but no capture backend available");
            return false;
#endif
        } else {
            LOG_ERROR("No display session detected (WAYLAND_DISPLAY and DISPLAY not set)");
            return false;
        }
    }

    // Create the selected backend
    switch (backend) {
        case CaptureBackendType::X11:
#ifdef HAVE_X11
            LOG_INFO("Creating X11 capture backend");
            m_capture = std::make_unique<X11Capture>();
            return m_capture->init(display);
#else
            LOG_ERROR("X11 capture not compiled in");
            return false;
#endif

        case CaptureBackendType::PIPEWIRE:
#ifdef HAVE_PIPEWIRE
            LOG_INFO("Creating PipeWire capture backend");
            m_capture = std::make_unique<PipeWireCapture>();
            return m_capture->init(nullptr);
#else
            LOG_ERROR("PipeWire capture not compiled in");
            return false;
#endif

        default:
            LOG_ERROR("Unknown capture backend type");
            return false;
    }
}

bool Server::init(const ServerConfig& config) {
    m_config = config;

    // Initialize capture backend
    if (!create_capture_backend(config.display.c_str())) {
        LOG_ERROR("Failed to initialize capture backend");
        return false;
    }

    LOG_INFO("Using %s capture backend", m_capture->get_name());

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
    }

    // Set input callback
    m_input_receiver->set_callback([this](const InputEvent& event) {
        handle_input(event);
    });

    LOG_INFO("Server initialized: %dx%d, waiting for client to configure stream...",
             m_capture->get_width(), m_capture->get_height());

    return true;
}

bool Server::init_encoder_from_client(const ClientInfo& client) {
    // Map client codec preference to CodecType
    CodecType codec = CodecType::AUTO;
    switch (client.codec) {
        case 1: codec = CodecType::AV1; break;
        case 2: codec = CodecType::HEVC; break;
        case 3: codec = CodecType::H264; break;
        default: codec = CodecType::AUTO; break;
    }

    // Map client quality mode
    QualityMode quality = QualityMode::AUTO;
    switch (client.quality_mode) {
        case 1: quality = QualityMode::LOW_LATENCY; break;
        case 2: quality = QualityMode::BALANCED; break;
        case 3: quality = QualityMode::HIGH_QUALITY; break;
        default: quality = QualityMode::AUTO; break;
    }

    int fps = client.fps;
    if (fps < 1) fps = 1;
    if (fps > 120) fps = 120;
    m_config.capture_fps = fps;

    // Update capture backend framerate (PipeWire will reconnect stream)
    m_capture->set_framerate(fps);

    // Auto-calculate bitrate if client sent 0
    int bitrate = static_cast<int>(client.bitrate);
    if (bitrate == 0) {
        switch (quality) {
            case QualityMode::AUTO:
                bitrate = (100000000LL * fps) / 60;
                break;
            case QualityMode::LOW_LATENCY:
                bitrate = (10000000LL * fps) / 60;
                break;
            case QualityMode::BALANCED:
                bitrate = (20000000LL * fps) / 60;
                break;
            case QualityMode::HIGH_QUALITY:
                bitrate = (100000000LL * fps) / 60;
                break;
        }
    }

    int gop_size = fps / 2;
    if (gop_size < 1) gop_size = 1;

    int cqp = client.cqp;
    if (cqp < 1) cqp = 1;
    if (cqp > 51) cqp = 51;

    // Update pacing mode
    m_config.pacing_mode = client.pacing_mode;
    if (quality == QualityMode::AUTO && m_config.pacing_mode == 0) {
        m_config.pacing_mode = 4;  // KEYFRAME pacing for AUTO quality
    }

    // Update audio config
    m_config.audio_enabled = (client.audio_enabled != 0);
    if (client.audio_bitrate > 0) {
        m_config.audio_bitrate = static_cast<int>(client.audio_bitrate);
        if (m_config.audio_bitrate < 16000) m_config.audio_bitrate = 16000;
        if (m_config.audio_bitrate > 510000) m_config.audio_bitrate = 510000;
    }

    // Create encoder
    EncoderConfig enc_config;
    enc_config.width = m_capture->get_width();
    enc_config.height = m_capture->get_height();
    enc_config.framerate = fps;
    enc_config.bitrate = bitrate;
    enc_config.gop_size = gop_size;
    enc_config.low_latency = (quality != QualityMode::HIGH_QUALITY && quality != QualityMode::AUTO);
    enc_config.quality_mode = quality;
    enc_config.codec_type = codec;
    enc_config.cqp = cqp;

    m_encoder = create_encoder(enc_config);
    if (!m_encoder) {
        LOG_ERROR("Failed to initialize hardware encoder");
        return false;
    }

    // Set keyframe callback now that encoder exists
    m_control->set_keyframe_callback([this]() {
        LOG_INFO("Keyframe requested by client");
        m_encoder->request_keyframe();
    });

    const char* codec_names[] = {"AV1", "HEVC", "H.264"};
    uint8_t actual_codec = m_encoder->get_codec_type();
    const char* codec_name = (actual_codec < 3) ? codec_names[actual_codec] : "unknown";

    printf("Encoder: %s %s | %d FPS | %d kbps\n",
           m_encoder->get_name(), codec_name, fps, bitrate / 1000);

#ifdef HAVE_OPUS
    // Initialize audio if client wants it
    m_audio_initialized = false;
    if (m_config.audio_enabled) {
        if (init_audio()) {
            LOG_INFO("Audio streaming enabled (%d kbps)", m_config.audio_bitrate / 1000);
        } else {
            LOG_WARN("Audio streaming disabled (initialization failed)");
        }
    }
#endif

    return true;
}

void Server::run() {
    m_running = true;

    while (m_running) {
        LOG_INFO("Waiting for client connection...");

        // Wait for client to connect and send its config
        ClientInfo client_info;
        if (!m_control->accept_client(client_info)) {
            if (!m_running) break;
            LOG_ERROR("Failed to accept client");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // Reconfigure the KWin virtual output (created by the portal during
        // Server::init) to match the tablet's native resolution and refresh
        // rate. Must happen BEFORE init_encoder_from_client — that call
        // reconnects the PipeWire stream via set_framerate(), which is what
        // picks up the new dimensions from KWin.
        if (client_info.width > 0 && client_info.height > 0 && client_info.fps > 0) {
            apply_virtual_output_mode(client_info.width, client_info.height, client_info.fps);
            save_display_mode(client_info.width, client_info.height, client_info.fps);
        }

        // Initialize encoder and audio based on client preferences
        if (!init_encoder_from_client(client_info)) {
            LOG_ERROR("Failed to initialize encoder with client config");
            m_control->reset();
            continue;
        }

        // Send configuration to client (with audio and codec info)
        uint8_t codec_type = m_encoder->get_codec_type();
#ifdef HAVE_OPUS
        int audio_port = m_audio_initialized ? m_config.audio_port : 0;
#else
        int audio_port = 0;
#endif
        m_control->send_config_full(m_capture->get_width(), m_capture->get_height(),
                                    m_config.video_port, m_config.input_port,
                                    audio_port, m_config.audio_sample_rate,
                                    m_config.audio_channels, m_config.audio_frame_ms,
                                    codec_type);

        // Set video destination with pacing mode
        PacingMode pacing = static_cast<PacingMode>(m_config.pacing_mode);
        m_video_sender->set_client(client_info.host, client_info.video_port, pacing);

#ifdef HAVE_OPUS
        // Set audio destination and start audio capture
        if (m_audio_initialized && m_audio_sender && m_audio_capture) {
            m_audio_sender->set_client(client_info.host, m_config.audio_port);
            m_audio_sequence = 0;
            m_audio_capture->start([this](const AudioFrame& frame) {
                on_audio_frame(frame);
            });
            LOG_INFO("Audio capture started for client");
        }
#endif

        // Sync uinput to the (possibly resized) capture dimensions so the
        // ABS touch/stylus range maps across the whole virtual output, not
        // the stale 1920x1080 the devices were created with.
        if (m_uinput && m_capture->get_width() > 0 && m_capture->get_height() > 0) {
            m_uinput->set_screen_size(m_capture->get_width(), m_capture->get_height());
        }

        // Initialize coordinate transform
        m_coord_transform.init(m_capture->get_width(), m_capture->get_height(),
                               client_info.width, client_info.height,
                               CoordTransform::Mode::LETTERBOX, false);

        printf("Client connected from %s - streaming started\n", client_info.host.c_str());
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
                if (capture_and_encode_loop()) {
                    next_frame += frame_interval;
                    // If we're behind, reset to now
                    if (next_frame < now) {
                        next_frame = now + frame_interval;
                    }
                }
                // If capture failed (no new frame from PipeWire), don't advance
                // — we'll try again on next iteration
            }

            // Calculate time until next frame and sleep smartly
            auto time_to_next = std::chrono::duration_cast<std::chrono::microseconds>(
                next_frame - std::chrono::high_resolution_clock::now());

            // For high FPS (>90), use tighter timing to avoid sleep overshooting
            if (m_config.capture_fps > 90) {
                // High FPS mode: sleep less aggressively, busy-wait for last 500us
                if (time_to_next.count() > 2000) {
                    // Sleep for 60% of remaining time (leaving margin for oversleep)
                    std::this_thread::sleep_for(time_to_next * 6 / 10);
                } else if (time_to_next.count() > 500) {
                    // Short sleep
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                // Busy wait for last 500us for accuracy
            } else {
                // Normal/Low FPS mode: can sleep more aggressively
                if (time_to_next.count() > 1000) {
                    std::this_thread::sleep_for(time_to_next / 2);
                } else if (time_to_next.count() > 100) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            }
        }

        if (m_running) {
            printf("Client disconnected - waiting for new connection...\n");
            LOG_INFO("Client disconnected, waiting for new connection...");
#ifdef HAVE_OPUS
            // Stop audio capture
            if (m_audio_capture && m_audio_capture->is_capturing()) {
                m_audio_capture->stop();
                LOG_INFO("Audio capture stopped");
            }
#endif
            // Release all pressed buttons/tools before resetting
            if (m_uinput && m_uinput->is_initialized()) {
                m_uinput->reset_all();
            }
            // Release encoder so next client can reconfigure
            m_encoder.reset();
#ifdef HAVE_OPUS
            m_opus_encoder.reset();
            m_audio_sender.reset();
            m_audio_capture.reset();
            m_audio_initialized = false;
#endif
            m_control->reset();
            m_input_receiver->reset();
        }
    }

    LOG_INFO("Server stopped");
}

bool Server::capture_and_encode_loop() {
    static auto last_timing_log = std::chrono::high_resolution_clock::now();
    static int capture_fail_count = 0;
    static int encode_fail_count = 0;
    static long total_capture_us = 0;
    static long total_encode_us = 0;
    static long total_send_us = 0;
    static int timing_count = 0;

    auto t0 = std::chrono::high_resolution_clock::now();

    // Capture frame
    CapturedFrame frame;
    if (!m_capture->capture_frame(frame)) {
        capture_fail_count++;
        return false;
    }

    auto t1 = std::chrono::high_resolution_clock::now();

    // Encode frame
    EncodedFrame encoded;
    if (!m_encoder->encode(frame.data, frame.width, frame.height, frame.stride,
                           frame.timestamp_us, encoded)) {
        encode_fail_count++;
        return false;
    }

    auto t2 = std::chrono::high_resolution_clock::now();

    // Send to client
    bool sent = m_video_sender->send_frame(encoded.data.data(), encoded.data.size(),
                               m_frame_count, encoded.is_keyframe, encoded.timestamp_us);

    auto t3 = std::chrono::high_resolution_clock::now();

    // Accumulate timing stats
    total_capture_us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    total_encode_us += std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
    total_send_us += std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
    timing_count++;

    // Log timing stats every 5 seconds
    auto now = std::chrono::high_resolution_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_timing_log).count() >= 5) {
        if (timing_count > 0) {
            LOG_INFO("Timing (avg): capture=%.2fms encode=%.2fms send=%.2fms | fails: capture=%d encode=%d | frames=%d",
                     total_capture_us / 1000.0 / timing_count,
                     total_encode_us / 1000.0 / timing_count,
                     total_send_us / 1000.0 / timing_count,
                     capture_fail_count, encode_fail_count, timing_count);
        }
        total_capture_us = total_encode_us = total_send_us = 0;
        capture_fail_count = encode_fail_count = timing_count = 0;
        last_timing_log = now;
    }

    if (m_frame_count % 60 == 0 || encoded.is_keyframe) {
        LOG_DEBUG("Frame %d: %zu bytes, keyframe=%d, sent=%d",
                  m_frame_count, encoded.data.size(), encoded.is_keyframe, sent);
    }
    m_frame_count++;
    return true;
}

void Server::handle_input(const InputEvent& event) {
    if (!m_uinput || !m_uinput->is_initialized()) {
        return;
    }

    // Transform coordinates
    int screen_x, screen_y;
    m_coord_transform.transform(event.x, event.y, screen_x, screen_y);

    // Debug: log event type
    static int hover_count = 0;
    if (event.type == InputEventType::STYLUS_HOVER) {
        if (hover_count++ % 30 == 0) {  // Log every 30th hover event
            LOG_DEBUG("STYLUS_HOVER: x=%.3f y=%.3f -> screen %d,%d",
                      event.x, event.y, screen_x, screen_y);
        }
    }

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

        case InputEventType::KEY_DOWN:
        case InputEventType::KEY_UP: {
            bool pressed = (event.type == InputEventType::KEY_DOWN);
            // Keycode is stored in the buttons field
            uint16_t keycode = event.buttons;
            m_uinput->send_key(keycode, pressed);
            LOG_DEBUG("Key event: keycode=%d pressed=%d", keycode, pressed);
            break;
        }

        case InputEventType::SCROLL: {
            // Scroll direction is stored in the y field (+1 = up, -1 = down)
            int direction = static_cast<int>(event.y);
            m_uinput->send_scroll(direction);
            LOG_DEBUG("Scroll event: direction=%d", direction);
            break;
        }

        default:
            break;
    }
}

void Server::stop() {
    m_running = false;

#ifdef HAVE_OPUS
    // Stop audio capture
    if (m_audio_capture && m_audio_capture->is_capturing()) {
        m_audio_capture->stop();
    }
#endif
}

#ifdef HAVE_OPUS
bool Server::init_audio() {
    // Create audio backend (auto-detect PipeWire/PulseAudio)
    m_audio_capture = create_audio_backend(AudioBackendType::AUTO);
    if (!m_audio_capture) {
        LOG_WARN("No audio backend available");
        return false;
    }

    // Initialize audio capture
    AudioConfig audio_config;
    audio_config.sample_rate = m_config.audio_sample_rate;
    audio_config.channels = m_config.audio_channels;
    audio_config.frame_size_ms = m_config.audio_frame_ms;

    if (!m_audio_capture->init(audio_config)) {
        LOG_WARN("Failed to initialize audio capture");
        m_audio_capture.reset();
        return false;
    }

    // Initialize Opus encoder
    OpusConfig opus_config;
    opus_config.sample_rate = m_config.audio_sample_rate;
    opus_config.channels = m_config.audio_channels;
    opus_config.bitrate = m_config.audio_bitrate;
    opus_config.frame_size_ms = m_config.audio_frame_ms;

    m_opus_encoder = std::make_unique<OpusEncoder>();
    if (!m_opus_encoder->init(opus_config)) {
        LOG_WARN("Failed to initialize Opus encoder");
        m_audio_capture.reset();
        return false;
    }

    // Initialize audio sender
    m_audio_sender = std::make_unique<AudioSender>();
    if (!m_audio_sender->init(m_config.audio_port)) {
        LOG_WARN("Failed to initialize audio sender");
        m_opus_encoder.reset();
        m_audio_capture.reset();
        return false;
    }

    m_audio_initialized = true;
    LOG_INFO("Audio initialized: %s backend, %dHz, %d channels, %dkbps",
             m_audio_capture->get_name(),
             m_config.audio_sample_rate,
             m_config.audio_channels,
             m_config.audio_bitrate / 1000);
    return true;
}

void Server::on_audio_frame(const AudioFrame& frame) {
    if (!m_opus_encoder || !m_audio_sender || !m_audio_sender->has_client()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_audio_mutex);

    // Debug: log audio frame reception periodically
    static int frame_count = 0;
    static auto last_log = std::chrono::steady_clock::now();
    frame_count++;

    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 5) {
        LOG_INFO("Audio: received %d frames, %d samples/frame, sent %lu packets, %lu bytes",
                 frame_count, frame.num_samples,
                 m_audio_sender->get_packets_sent(),
                 m_audio_sender->get_bytes_sent());
        frame_count = 0;
        last_log = now;
    }

    // Encode the audio frame (buffers internally, calls callback for each complete frame)
    m_opus_encoder->encode(frame.samples, frame.num_samples, frame.timestamp_us,
        [this](const EncodedAudio& encoded) {
            // Send the encoded packet
            m_audio_sender->send_packet(encoded.data.data(), encoded.data.size(),
                                         m_audio_sequence++, encoded.timestamp_us);
        });
}
#endif

}  // namespace stream_tablet
