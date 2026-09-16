// Standalone fps probe for the PipeWire capture path.
// Mirrors the real Server flow: init portal -> apply_virtual_output_mode ->
// set_framerate (reconnects pw_stream). This is what a tablet client does
// when it connects, so the second negotiated format is the one that matters.

#include "pipewire_capture.hpp"
#include "../display/kwin_output.hpp"
#include "../util/logger.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    using namespace stream_tablet;
    Logger::set_level(LogLevel::INFO);

    // Defaults match the saved tablet display config (2960x1848@120).
    int width  = (argc > 1) ? atoi(argv[1]) : 2960;
    int height = (argc > 2) ? atoi(argv[2]) : 1848;
    int fps    = (argc > 3) ? atoi(argv[3]) : 120;
    int seconds = (argc > 4) ? atoi(argv[4]) : 8;

    PipeWireCapture cap;
    if (!cap.init(nullptr)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    fprintf(stderr, "\n--- Step 2: applying virtual output mode %dx%d@%d, then set_framerate(%d)\n",
            width, height, fps, fps);
    apply_virtual_output_mode(width, height, fps);
    cap.set_framerate(fps);

    fprintf(stderr, "\n--- Step 3: probing for %d seconds. Generate damage on the captured output.\n",
            seconds);
    auto t0 = std::chrono::steady_clock::now();
    long iterations = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
        CapturedFrame f;
        cap.capture_frame(f);
        iterations++;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "Probe done. %.1fs, iterations=%ld. See \"PipeWire delivered fps\" lines for the new-frame rate from KWin.\n",
            elapsed / 1000.0, iterations);

    cap.shutdown();
    return 0;
}
