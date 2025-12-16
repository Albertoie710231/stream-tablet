#include "audio_backend.hpp"
#include "util/logger.hpp"

#ifdef HAVE_PIPEWIRE
#include "pipewire_audio.hpp"
#endif

#ifdef HAVE_PULSE
#include "pulseaudio_audio.hpp"
#endif

namespace stream_tablet {

std::unique_ptr<AudioBackend> create_audio_backend(AudioBackendType type) {
    std::unique_ptr<AudioBackend> backend;

    switch (type) {
        case AudioBackendType::AUTO:
            // Try PipeWire first, fall back to PulseAudio
#ifdef HAVE_PIPEWIRE
            LOG_INFO("Trying PipeWire audio backend...");
            backend = std::make_unique<PipeWireAudio>();
            return backend;
#endif
#ifdef HAVE_PULSE
            LOG_INFO("Trying PulseAudio audio backend...");
            backend = std::make_unique<PulseAudioAudio>();
            return backend;
#endif
            LOG_ERROR("No audio backend available");
            return nullptr;

        case AudioBackendType::PIPEWIRE:
#ifdef HAVE_PIPEWIRE
            return std::make_unique<PipeWireAudio>();
#else
            LOG_ERROR("PipeWire audio backend not available (not compiled in)");
            return nullptr;
#endif

        case AudioBackendType::PULSEAUDIO:
#ifdef HAVE_PULSE
            return std::make_unique<PulseAudioAudio>();
#else
            LOG_ERROR("PulseAudio audio backend not available (not compiled in)");
            return nullptr;
#endif
    }

    return nullptr;
}

}  // namespace stream_tablet
