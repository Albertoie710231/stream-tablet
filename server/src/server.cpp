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
    // Join VideoSender's feedback thread here rather than relying on member
    // destruction order: it holds a callback bound to this Server, so it must
    // stop before any member it might touch is gone.
    if (m_video_sender) {
        m_video_sender->shutdown();
    }
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

    // Advertise the server on mDNS so the tablet can auto-discover it.
    // Non-fatal if avahi isn't installed — user can still type the IP.
    m_mdns.start(config.control_port);

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

    // Keyframe requests only raise a flag. The UDP feedback path runs on
    // VideoSender's own thread, and m_encoder is created and destroyed around
    // each client session on the main loop — dereferencing it from there was a
    // use-after-free waiting for the client to start sending feedback packets.
    m_control->set_keyframe_callback([this]() {
        LOG_DEBUG("Keyframe requested by client (TCP control)");
        m_keyframe_requested.store(true, std::memory_order_relaxed);
    });
    m_video_sender->set_keyframe_request_callback([this]() {
        LOG_DEBUG("Keyframe requested by client (UDP feedback)");
        m_keyframe_requested.store(true, std::memory_order_relaxed);
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

    // Keyframe interval. fps/2 means two forced keyframes a second, and at
    // 2960x1848 an AV1 intra frame is ~33x the size of an inter frame — that
    // put 36% of all bandwidth into keyframes. The client already asks for a
    // keyframe when it detects an incomplete frame (and the server honours it
    // on both the control channel and the UDP feedback path), so forcing them
    // this often buys very little. Default to one every 2 seconds.
    int gop_seconds = 2;
    if (const char* e = std::getenv("STREAM_TABLET_GOP_SECONDS")) gop_seconds = atoi(e);
    if (gop_seconds < 1) gop_seconds = 1;
    int gop_size = fps * gop_seconds;
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
    // Hand the negotiated DMA-BUF description to the encoder so it can build a
    // zero-copy import pipeline instead of converting BGRA on the CPU.
    enc_config.dmabuf_input = m_capture->is_dmabuf_capture();
    enc_config.drm_format = m_capture->get_drm_format();
    enc_config.drm_modifier = m_capture->get_drm_modifier();

    m_encoder = create_encoder(enc_config);
    if (!m_encoder) {
        LOG_ERROR("Failed to initialize hardware encoder");
        return false;
    }

    m_encoder_config = enc_config;

    // If capture negotiated DMA-BUF but the encoder could not build an import
    // pipeline, every frame would be rejected. Drop capture back to CPU buffers
    // and rebuild the encoder against them.
    if (enc_config.dmabuf_input && !m_encoder->uses_dmabuf_input()) {
        if (!fall_back_to_cpu_capture()) return false;
    }

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

bool Server::fall_back_to_cpu_capture() {
    m_encoder.reset();
    m_capture->disable_dmabuf();

    m_encoder_config.dmabuf_input = m_capture->is_dmabuf_capture();
    m_encoder_config.drm_format = m_capture->get_drm_format();
    m_encoder_config.drm_modifier = m_capture->get_drm_modifier();
    m_encoder_config.width = m_capture->get_width();
    m_encoder_config.height = m_capture->get_height();

    m_encoder = create_encoder(m_encoder_config);
    if (!m_encoder) {
        LOG_ERROR("Failed to initialize hardware encoder on CPU fallback path");
        return false;
    }
    m_encoder->request_keyframe();
    LOG_WARN("Recovered onto the CPU capture path");
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
        // Allow pacing at most a third of a frame interval, so a large keyframe
        // cannot stall the capture loop for multiple frames.
        m_video_sender->set_max_pacing_us(1000000L / std::max(1, m_config.capture_fps) / 3);
        m_video_sender->set_client(client_info.host, client_info.video_port, pacing);

#ifdef HAVE_OPUS
        // Set audio destination and start audio capture
        if (m_audio_initialized && m_audio_sender && m_audio_capture) {
            // If the client requested exclusive audio, route everything through
            // a null sink BEFORE starting capture — then the audio backend's
            // default-monitor connection picks up the null sink's monitor
            // instead of the real speakers.
            if (client_info.audio_enabled && client_info.audio_exclusive) {
                m_audio_router.begin();
            }

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
        m_keyframe_requested.store(false, std::memory_order_relaxed);
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
            // Restore the system default sink and unload the null sink.
            m_audio_router.end();
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
    static auto last_frame_t = std::chrono::high_resolution_clock::time_point{};
    static int capture_fail_count = 0;
    static int encode_fail_count = 0;
    static long total_capture_us = 0;
    static long total_encode_us = 0;
    static long total_send_us = 0;
    static int timing_count = 0;
    static long max_gap_us = 0;
    static long sum_gap_us = 0;
    static long sum_gap_sq_us = 0;  // for stddev
    static long over20ms_gaps = 0;  // count of >20ms inter-frame gaps (perceptible stutters)

    auto t0 = std::chrono::high_resolution_clock::now();

    // Honour any keyframe request raised since the last frame. Done here so the
    // encoder is only ever touched from this thread.
    if (m_keyframe_requested.exchange(false, std::memory_order_relaxed)) {
        LOG_INFO("Keyframe requested by client");
        m_encoder->request_keyframe();
    }

    // Capture frame
    CapturedFrame frame;
    if (!m_capture->capture_frame(frame)) {
        capture_fail_count++;
        return false;
    }

    auto t1 = std::chrono::high_resolution_clock::now();

    // Encode frame (zero-copy when the capture backend gave us a DMA-BUF)
    EncodedFrame encoded;
    if (!m_encoder->encode_frame(frame, encoded)) {
        encode_fail_count++;
        // A zero-copy frame that will not import is unrecoverable per-frame: the
        // pixels only exist on the GPU, so there is no CPU path to retry with.
        // After a short grace period (the encoder legitimately returns EAGAIN
        // while its async pipeline fills) renegotiate capture to CPU buffers
        // rather than streaming nothing.
        if (frame.is_dmabuf && m_encoder->uses_dmabuf_input()) {
            if (++m_dmabuf_encode_failures == 30) {
                LOG_ERROR("dmabuf: %u consecutive encode failures — abandoning "
                          "the zero-copy path", m_dmabuf_encode_failures);
                fall_back_to_cpu_capture();
            }
        }
        return false;
    }
    m_dmabuf_encode_failures = 0;

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

    // Inter-frame gap (wall-clock between successive sends). Smooth 100 fps
    // means avg=10ms with low max; bursty 100 fps shows up as max>>avg.
    if (last_frame_t.time_since_epoch().count() != 0) {
        long gap_us = std::chrono::duration_cast<std::chrono::microseconds>(
            t3 - last_frame_t).count();
        sum_gap_us += gap_us;
        sum_gap_sq_us += gap_us * gap_us / 1000;  // scale to avoid overflow
        if (gap_us > max_gap_us) max_gap_us = gap_us;
        if (gap_us > 20000) over20ms_gaps++;
    }
    last_frame_t = t3;

    // Log timing stats every 5 seconds
    auto now = std::chrono::high_resolution_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_timing_log).count() >= 5) {
        // Wire throughput over the window — the number that says whether a
        // missed frame rate is the encoder's fault or the network's.
        static uint64_t last_bytes = 0;
        uint64_t bytes_now = m_video_sender->get_bytes_sent();
        uint64_t delta_bytes = bytes_now - last_bytes;
        last_bytes = bytes_now;
        double window_s = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_timing_log).count() / 1000.0;
        if (timing_count > 0 && window_s > 0) {
            LOG_INFO("Wire: %.1f Mbps | %.1f KB/frame | %.1f fps delivered",
                     delta_bytes * 8.0 / window_s / 1e6,
                     delta_bytes / 1024.0 / timing_count,
                     timing_count / window_s);
        }
        if (timing_count > 0) {
            double avg_gap_ms = sum_gap_us / 1000.0 / std::max(1, timing_count - 1);
            // stddev_ms = sqrt(E[g^2]-E[g]^2); E[g^2] reconstructed from scaled accumulator
            double mean_g_us = (timing_count > 1) ? double(sum_gap_us) / (timing_count - 1) : 0.0;
            double mean_gsq = (timing_count > 1) ? double(sum_gap_sq_us) * 1000.0 / (timing_count - 1) : 0.0;
            double var = mean_gsq - mean_g_us * mean_g_us;
            double std_ms = (var > 0) ? std::sqrt(var) / 1000.0 : 0.0;
            LOG_INFO("Timing (avg): capture=%.2fms encode=%.2fms send=%.2fms | gap avg=%.2fms max=%.2fms std=%.2fms stutters>20ms=%ld | fails: capture=%d encode=%d | frames=%d",
                     total_capture_us / 1000.0 / timing_count,
                     total_encode_us / 1000.0 / timing_count,
                     total_send_us / 1000.0 / timing_count,
                     avg_gap_ms,
                     max_gap_us / 1000.0,
                     std_ms,
                     over20ms_gaps,
                     capture_fail_count, encode_fail_count, timing_count);
        }
        total_capture_us = total_encode_us = total_send_us = 0;
        capture_fail_count = encode_fail_count = timing_count = 0;
        max_gap_us = sum_gap_us = sum_gap_sq_us = over20ms_gaps = 0;
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
