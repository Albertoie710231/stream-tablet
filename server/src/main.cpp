#include "server.hpp"
#include "util/logger.hpp"
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <getopt.h>

using namespace stream_tablet;

static Server* g_server = nullptr;
static volatile sig_atomic_t g_signal_count = 0;

static void signal_handler(int sig) {
    g_signal_count = g_signal_count + 1;
    int count = g_signal_count;

    if (count == 1) {
        // First signal - request graceful shutdown
        // Note: LOG_INFO is not async-signal-safe, but acceptable for debugging
        LOG_INFO("Received signal %d, shutting down...", sig);
        if (g_server) {
            g_server->stop();
        }
    } else {
        // Second signal - force exit but still run destructors
        LOG_INFO("Received second signal, forcing exit...");
        // Use exit() instead of _exit() to allow destructors to run
        // This is important to properly release uinput device and avoid terminal corruption
        exit(0);
    }
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -d, --display DISPLAY   X11 display (default: :0)\n");
    printf("  -c, --capture BACKEND   Capture backend: auto, x11, pipewire (default: auto)\n");
    printf("  -e, --encoder CODEC     Video codec: auto, av1, hevc, h264 (default: auto)\n");
    printf("  -f, --fps FPS           Capture FPS, 1-120 (default: 60)\n");
    printf("  -b, --bitrate BPS       Bitrate in bps (default: auto based on fps/quality)\n");
    printf("  -g, --gop SIZE          GOP size / keyframe interval (default: fps/2)\n");
    printf("  -q, --quality MODE      Quality mode: auto, low, balanced, high (default: auto)\n");
    printf("                          auto = adaptive CQP (sharp text + smooth motion)\n");
    printf("  -Q, --cqp VALUE         CQP quality value for auto/high mode, 1-51 (default: 24)\n");
    printf("  -P, --pacing MODE       Pacing mode: auto, none, light, aggressive, keyframe (default: auto)\n");
    printf("  -p, --port PORT         Control port (default: 9500)\n");
    printf("  -A, --no-audio          Disable audio streaming\n");
    printf("  -a, --audio-bitrate BPS Audio bitrate in bps (default: 128000)\n");
    printf("  -v, --verbose           Enable info logging (use -vv for debug)\n");
    printf("  -h, --help              Show this help\n");
    printf("\nCapture backends:\n");
    printf("  auto      Auto-detect based on session (Wayland->PipeWire, X11->X11)\n");
#ifdef HAVE_X11
    printf("  x11       X11/XCB screen capture (works on X11 and Xwayland)\n");
#endif
#ifdef HAVE_PIPEWIRE
    printf("  pipewire  PipeWire/Portal screen capture (native Wayland)\n");
#endif
    printf("\nQuality modes:\n");
    printf("  low       Low latency CBR - minimal delay, lower quality\n");
    printf("  balanced  Balanced CBR - good quality with reasonable latency\n");
    printf("  high      High quality CQP - best quality, uses more bandwidth\n");
    printf("\nPacing modes:\n");
    printf("  auto      Auto-detect based on IP (default)\n");
    printf("  none      No pacing - fastest, use for fast local networks\n");
    printf("  light     Light pacing - for WiFi connections\n");
    printf("  aggressive Aggressive pacing - for slow USB tethering\n");
    printf("\nVideo codecs:\n");
    printf("  auto      Auto-select best available (AV1 > HEVC > H.264)\n");
    printf("  av1       AV1 - best quality/compression, slower encoding\n");
    printf("  hevc      HEVC/H.265 - faster encoding, good quality (recommended for gaming)\n");
    printf("  h264      H.264 - fastest encoding, widest compatibility\n");
}

int main(int argc, char* argv[]) {
    ServerConfig config;
    CaptureBackendType backend_type = CaptureBackendType::AUTO;

    static struct option long_options[] = {
        {"display", required_argument, 0, 'd'},
        {"capture", required_argument, 0, 'c'},
        {"encoder", required_argument, 0, 'e'},
        {"fps", required_argument, 0, 'f'},
        {"bitrate", required_argument, 0, 'b'},
        {"gop", required_argument, 0, 'g'},
        {"quality", required_argument, 0, 'q'},
        {"cqp", required_argument, 0, 'Q'},
        {"pacing", required_argument, 0, 'P'},
        {"port", required_argument, 0, 'p'},
        {"no-audio", no_argument, 0, 'A'},
        {"audio-bitrate", required_argument, 0, 'a'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    bool bitrate_set = false;
    bool gop_set = false;
    int verbosity = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "d:c:e:f:b:g:q:Q:P:p:Aa:vh", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'd':
                config.display = optarg;
                break;
            case 'c':
                if (strcmp(optarg, "auto") == 0) {
                    backend_type = CaptureBackendType::AUTO;
                } else if (strcmp(optarg, "x11") == 0) {
                    backend_type = CaptureBackendType::X11;
                } else if (strcmp(optarg, "pipewire") == 0 || strcmp(optarg, "pw") == 0) {
                    backend_type = CaptureBackendType::PIPEWIRE;
                } else {
                    fprintf(stderr, "Unknown capture backend: %s\n", optarg);
                    print_usage(argv[0]);
                    return 1;
                }
                break;
            case 'e':
                if (strcmp(optarg, "auto") == 0) {
                    config.codec_type = CodecType::AUTO;
                } else if (strcmp(optarg, "av1") == 0) {
                    config.codec_type = CodecType::AV1;
                } else if (strcmp(optarg, "hevc") == 0 || strcmp(optarg, "h265") == 0) {
                    config.codec_type = CodecType::HEVC;
                } else if (strcmp(optarg, "h264") == 0 || strcmp(optarg, "avc") == 0) {
                    config.codec_type = CodecType::H264;
                } else {
                    fprintf(stderr, "Unknown video codec: %s\n", optarg);
                    print_usage(argv[0]);
                    return 1;
                }
                break;
            case 'f':
                config.capture_fps = atoi(optarg);
                if (config.capture_fps < 1) config.capture_fps = 1;
                if (config.capture_fps > 120) config.capture_fps = 120;
                break;
            case 'b':
                config.bitrate = atoi(optarg);
                bitrate_set = true;
                break;
            case 'g':
                config.gop_size = atoi(optarg);
                gop_set = true;
                break;
            case 'q':
                if (strcmp(optarg, "auto") == 0) {
                    config.quality_mode = QualityMode::AUTO;
                } else if (strcmp(optarg, "low") == 0) {
                    config.quality_mode = QualityMode::LOW_LATENCY;
                } else if (strcmp(optarg, "balanced") == 0) {
                    config.quality_mode = QualityMode::BALANCED;
                } else if (strcmp(optarg, "high") == 0) {
                    config.quality_mode = QualityMode::HIGH_QUALITY;
                } else {
                    fprintf(stderr, "Unknown quality mode: %s\n", optarg);
                    print_usage(argv[0]);
                    return 1;
                }
                break;
            case 'Q':
                config.cqp = atoi(optarg);
                if (config.cqp < 1) config.cqp = 1;
                if (config.cqp > 51) config.cqp = 51;
                break;
            case 'P':
                if (strcmp(optarg, "auto") == 0) {
                    config.pacing_mode = 0;
                } else if (strcmp(optarg, "none") == 0) {
                    config.pacing_mode = 1;
                } else if (strcmp(optarg, "light") == 0) {
                    config.pacing_mode = 2;
                } else if (strcmp(optarg, "aggressive") == 0) {
                    config.pacing_mode = 3;
                } else if (strcmp(optarg, "keyframe") == 0) {
                    config.pacing_mode = 4;
                } else {
                    fprintf(stderr, "Unknown pacing mode: %s\n", optarg);
                    print_usage(argv[0]);
                    return 1;
                }
                break;
            case 'p':
                config.control_port = static_cast<uint16_t>(atoi(optarg));
                config.video_port = config.control_port + 1;
                config.input_port = config.control_port + 2;
                config.audio_port = config.control_port + 3;
                break;
            case 'A':
                config.audio_enabled = false;
                break;
            case 'a':
                config.audio_bitrate = atoi(optarg);
                if (config.audio_bitrate < 16000) config.audio_bitrate = 16000;
                if (config.audio_bitrate > 510000) config.audio_bitrate = 510000;
                break;
            case 'v':
                verbosity++;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Apply verbosity level: -v = INFO, -vv = DEBUG
    if (verbosity >= 2) {
        Logger::set_level(LogLevel::DEBUG);
    } else if (verbosity == 1) {
        Logger::set_level(LogLevel::INFO);
    }
    // Default is WARN (quiet mode)

    // Auto-calculate bitrate based on FPS and quality mode if not explicitly set
    if (!bitrate_set) {
        switch (config.quality_mode) {
            case QualityMode::AUTO:
                config.bitrate = (100000000LL * config.capture_fps) / 60;  // 100 Mbps base (CQP mode)
                break;
            case QualityMode::LOW_LATENCY:
                config.bitrate = (10000000LL * config.capture_fps) / 60;  // 10 Mbps base
                break;
            case QualityMode::BALANCED:
                config.bitrate = (20000000LL * config.capture_fps) / 60;  // 20 Mbps base
                break;
            case QualityMode::HIGH_QUALITY:
                config.bitrate = (100000000LL * config.capture_fps) / 60;  // 100 Mbps base
                break;
        }
    }

    // Auto-calculate GOP size if not set (keyframe every ~0.5 seconds for fast recovery)
    if (!gop_set) {
        config.gop_size = config.capture_fps / 2;
        if (config.gop_size < 1) config.gop_size = 1;
    }

    // For AUTO mode, use keyframe pacing by default if not explicitly set
    if (config.quality_mode == QualityMode::AUTO && config.pacing_mode == 0) {
        config.pacing_mode = 4;  // KEYFRAME pacing
    }

    const char* quality_str = "auto";
    if (config.quality_mode == QualityMode::LOW_LATENCY) quality_str = "low";
    else if (config.quality_mode == QualityMode::BALANCED) quality_str = "balanced";
    else if (config.quality_mode == QualityMode::HIGH_QUALITY) quality_str = "high";

    const char* codec_str = "auto";
    if (config.codec_type == CodecType::AV1) codec_str = "AV1";
    else if (config.codec_type == CodecType::HEVC) codec_str = "HEVC";
    else if (config.codec_type == CodecType::H264) codec_str = "H.264";

    // Always show startup info (regardless of verbosity)
    printf("StreamTablet Server v1.0.0\n");
    printf("Codec: %s | Quality: %s", codec_str, quality_str);
    if (config.quality_mode == QualityMode::HIGH_QUALITY || config.quality_mode == QualityMode::AUTO) {
        printf(" (CQP: %d)", config.cqp);
    }
    printf(" | %d FPS | Port: %d", config.capture_fps, config.control_port);
#ifdef HAVE_OPUS
    if (config.audio_enabled) {
        printf(" | Audio: %dkbps", config.audio_bitrate / 1000);
    } else {
        printf(" | Audio: off");
    }
#endif
    printf("\n");
    printf("Waiting for connection... (use -v for detailed logs)\n");

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create and run server
    Server server;
    g_server = &server;

    // Set capture backend type before init
    server.set_capture_backend(backend_type);

    if (!server.init(config)) {
        LOG_ERROR("Failed to initialize server");
        return 1;
    }

    server.run();

    g_server = nullptr;
    LOG_INFO("Server exited");
    return 0;
}
