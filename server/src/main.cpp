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
    printf("  -f, --fps FPS           Capture FPS, 1-120 (default: 60)\n");
    printf("  -b, --bitrate BPS       Bitrate in bps (default: auto based on fps)\n");
    printf("  -g, --gop SIZE          GOP size / keyframe interval (default: fps/2)\n");
    printf("  -p, --port PORT         Control port (default: 9500)\n");
    printf("  -v, --verbose           Enable debug logging\n");
    printf("  -h, --help              Show this help\n");
}

int main(int argc, char* argv[]) {
    ServerConfig config;

    static struct option long_options[] = {
        {"display", required_argument, 0, 'd'},
        {"fps", required_argument, 0, 'f'},
        {"bitrate", required_argument, 0, 'b'},
        {"gop", required_argument, 0, 'g'},
        {"port", required_argument, 0, 'p'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    bool bitrate_set = false;
    bool gop_set = false;

    int opt;
    while ((opt = getopt_long(argc, argv, "d:f:b:g:p:vh", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'd':
                config.display = optarg;
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

    // Auto-calculate bitrate based on FPS if not explicitly set
    // Base: 15 Mbps at 60fps, scale linearly
    if (!bitrate_set) {
        config.bitrate = (15000000 * config.capture_fps) / 60;
    }

    // Auto-calculate GOP size if not set (keyframe every ~0.5 seconds for fast recovery)
    if (!gop_set) {
        config.gop_size = config.capture_fps / 2;
        if (config.gop_size < 1) config.gop_size = 1;
    }

    LOG_INFO("StreamTablet Server v1.0.0");
    LOG_INFO("Display: %s, FPS: %d, Bitrate: %.1f Mbps, GOP: %d",
             config.display.c_str(), config.capture_fps,
             config.bitrate / 1000000.0, config.gop_size);
    LOG_INFO("Ports: control=%d, video=%d, input=%d",
             config.control_port, config.video_port, config.input_port);

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create and run server
    Server server;
    g_server = &server;

    if (!server.init(config)) {
        LOG_ERROR("Failed to initialize server");
        return 1;
    }

    server.run();

    g_server = nullptr;
    LOG_INFO("Server exited");
    return 0;
}
