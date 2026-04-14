#pragma once

#include <string>

namespace stream_tablet {

// Routes all PC audio into a dedicated null sink so that, while the tablet
// is streaming, the PC speakers stay silent and only the tablet hears sound.
//
// Implemented as a thin wrapper around `pactl` (which talks to both
// PulseAudio and PipeWire's pulseaudio compatibility layer). Not linked
// against libpulse to keep the build simple and avoid a new dependency.
class AudioRouter {
public:
    AudioRouter() = default;
    ~AudioRouter();

    // Create the null sink, make it the default, and move existing streams
    // onto it. Remembers the previous default sink so end() can restore it.
    bool begin();

    // Restore the previous default, move streams back, unload the null sink.
    // Safe to call repeatedly.
    void end();

    bool is_active() const { return m_active; }

    // Unload any leftover `stream_tablet_sink` from a previous crashed run.
    // Call once at server startup.
    static void cleanup_stale();

private:
    bool m_active = false;
    std::string m_original_default_sink;
    std::string m_module_id;  // id returned by `pactl load-module`
};

}  // namespace stream_tablet
