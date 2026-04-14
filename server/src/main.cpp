#include "server.hpp"
#include "util/logger.hpp"
#ifdef HAVE_OPUS
#include "audio/audio_router.hpp"
#endif
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
        LOG_INFO("Received signal %d, shutting down...", sig);
        if (g_server) {
            g_server->stop();
        }
    } else {
        LOG_INFO("Received second signal, forcing exit...");
        exit(0);
    }
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -d, --display DISPLAY   X11 display (default: :0)\n");
    printf("  -c, --capture BACKEND   Capture backend: auto, x11, pipewire (default: auto)\n");
    printf("  -o, --output NAME       Output to mirror, e.g., DP-3 (default: first output)\n");
    printf("  -V, --virtual WxH       Create virtual output, e.g., 2800x1752\n");
    printf("  -p, --port PORT         Control port (default: 9500)\n");
    printf("  -v, --verbose           Enable info logging (use -vv for debug)\n");
    printf("  -h, --help              Show this help\n");
    printf("\nStreaming settings (codec, fps, quality, etc.) are configured from the tablet app.\n");
    printf("\nExamples:\n");
    printf("  %s                           # Mirror first output\n", prog);
    printf("  %s -o DP-3                   # Mirror specific output\n", prog);
    printf("  %s -V 2800x1752              # Create virtual output at tablet resolution\n", prog);
}

int main(int argc, char* argv[]) {
    ServerConfig config;
    CaptureBackendType backend_type = CaptureBackendType::AUTO;

    static struct option long_options[] = {
        {"display", required_argument, 0, 'd'},
        {"capture", required_argument, 0, 'c'},
        {"output", required_argument, 0, 'o'},
        {"virtual", required_argument, 0, 'V'},
        {"port", required_argument, 0, 'p'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int verbosity = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "d:c:o:V:p:vh", long_options, nullptr)) != -1) {
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
            case 'o':
                config.output_name = optarg;
                break;
            case 'V': {
                int w = 0, h = 0;
                if (sscanf(optarg, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                    config.virtual_width = w;
                    config.virtual_height = h;
                } else {
                    fprintf(stderr, "Invalid virtual output size: %s (use WxH, e.g., 2800x1752)\n", optarg);
                    return 1;
                }
                break;
            }
            case 'p':
                config.control_port = static_cast<uint16_t>(atoi(optarg));
                config.video_port = config.control_port + 1;
                config.input_port = config.control_port + 2;
                config.audio_port = config.control_port + 3;
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

    if (verbosity >= 2) {
        Logger::set_level(LogLevel::DEBUG);
    } else if (verbosity == 1) {
        Logger::set_level(LogLevel::INFO);
    }

    printf("StreamTablet Server v1.2.0\n");
    if (config.virtual_width > 0) {
        printf("Virtual output: %dx%d | Port: %d\n",
               config.virtual_width, config.virtual_height, config.control_port);
    } else if (!config.output_name.empty()) {
        printf("Output: %s | Port: %d\n", config.output_name.c_str(), config.control_port);
    } else {
        printf("Port: %d | Waiting for tablet to connect...\n", config.control_port);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

#ifdef HAVE_OPUS
    // If a previous run crashed, its null sink may still be loaded and
    // holding the system default — clean it up before we start.
    AudioRouter::cleanup_stale();
#endif

    Server server;
    g_server = &server;

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
