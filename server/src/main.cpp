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
    printf("  -f, --fps FPS           Capture FPS, 1-120 (default: 60)\n");
    printf("  -b, --bitrate BPS       Bitrate in bps (default: auto based on fps/quality)\n");
    printf("  -g, --gop SIZE          GOP size / keyframe interval (default: fps/2)\n");
    printf("  -q, --quality MODE      Quality mode: low, balanced, high (default: balanced)\n");
    printf("  -Q, --cqp VALUE         CQP quality value for high mode, 1-51 (default: 20)\n");
    printf("  -p, --port PORT         Control port (default: 9500)\n");
    printf("  -v, --verbose           Enable debug logging\n");
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
}

int main(int argc, char* argv[]) {
    ServerConfig config;
    CaptureBackendType backend_type = CaptureBackendType::AUTO;

    static struct option long_options[] = {
        {"display", required_argument, 0, 'd'},
        {"capture", required_argument, 0, 'c'},
        {"fps", required_argument, 0, 'f'},
        {"bitrate", required_argument, 0, 'b'},
        {"gop", required_argument, 0, 'g'},
        {"quality", required_argument, 0, 'q'},
        {"cqp", required_argument, 0, 'Q'},
        {"port", required_argument, 0, 'p'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    bool bitrate_set = false;
    bool gop_set = false;

    int opt;
    while ((opt = getopt_long(argc, argv, "d:c:f:b:g:q:Q:p:vh", long_options, nullptr)) != -1) {
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
                if (strcmp(optarg, "low") == 0) {
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
            case 'p':
                config.control_port = static_cast<uint16_t>(atoi(optarg));
                config.video_port = config.control_port + 1;
                config.input_port = config.control_port + 2;
                break;
            case 'v':
                Logger::set_level(LogLevel::DEBUG);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Auto-calculate bitrate based on FPS and quality mode if not explicitly set
    if (!bitrate_set) {
        switch (config.quality_mode) {
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

    const char* quality_str = "balanced";
    if (config.quality_mode == QualityMode::LOW_LATENCY) quality_str = "low";
    else if (config.quality_mode == QualityMode::HIGH_QUALITY) quality_str = "high";

    LOG_INFO("StreamTablet Server v1.0.0");
    LOG_INFO("Display: %s, FPS: %d, Bitrate: %.1f Mbps, GOP: %d",
             config.display.c_str(), config.capture_fps,
             config.bitrate / 1000000.0, config.gop_size);
    LOG_INFO("Quality: %s%s", quality_str,
             config.quality_mode == QualityMode::HIGH_QUALITY ?
             (", CQP: " + std::to_string(config.cqp)).c_str() : "");
    LOG_INFO("Ports: control=%d, video=%d, input=%d",
             config.control_port, config.video_port, config.input_port);

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
