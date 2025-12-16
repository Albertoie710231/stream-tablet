#pragma once

#include <cstdint>
#include <string>

namespace stream_tablet {

enum class QualityMode {
    AUTO,          // Adaptive: CQP with dynamic adjustment based on network conditions
    LOW_LATENCY,   // CBR, optimized for latency
    BALANCED,      // CBR, balanced quality/latency
    HIGH_QUALITY   // CQP, optimized for quality (manual tuning)
};

struct ServerConfig {
    // Display
    std::string display = ":0";
    int capture_fps = 60;

    // Encoding
    int bitrate = 15000000;  // 15 Mbps
    int gop_size = 60;       // Keyframe every 1 second at 60fps
    QualityMode quality_mode = QualityMode::AUTO;
    int cqp = 24;            // Quality level for CQP mode (lower = better, 1-51)
                             // AUTO mode starts at 24 and adjusts dynamically

    // Network
    uint16_t control_port = 9500;
    uint16_t video_port = 9501;
    uint16_t input_port = 9502;
    uint16_t audio_port = 9503;

    // Audio
    bool audio_enabled = true;
    int audio_sample_rate = 48000;
    int audio_channels = 2;
    int audio_bitrate = 128000;    // Opus bitrate in bps
    int audio_frame_ms = 10;       // Frame size in milliseconds

    // Security
    std::string cert_file = "server.crt";
    std::string key_file = "server.key";
    std::string ca_file = "ca.crt";

    // Target resolution (0 = use screen resolution)
    int target_width = 0;
    int target_height = 0;

    // Pacing mode for video sender (0=auto, 1=none, 2=light, 3=aggressive)
    int pacing_mode = 0;  // AUTO by default
};

struct EncoderConfig {
    int width = 0;
    int height = 0;
    int framerate = 60;
    int bitrate = 15000000;
    int gop_size = 60;
    bool low_latency = true;
    QualityMode quality_mode = QualityMode::BALANCED;
    int cqp = 20;  // Quality level for CQP mode (lower = better, 1-51)
};

// Protocol constants
constexpr uint16_t PROTOCOL_MAGIC = 0x5354;  // "ST" for video
constexpr uint16_t AUDIO_PROTOCOL_MAGIC = 0x5341;  // "SA" for audio
constexpr uint8_t PACKET_TYPE_VIDEO = 0x01;
constexpr uint8_t PACKET_TYPE_KEYFRAME = 0x02;
constexpr uint8_t PACKET_TYPE_CONFIG = 0x03;
constexpr uint8_t PACKET_TYPE_AUDIO = 0x04;

constexpr size_t MAX_PACKET_PAYLOAD = 1200;  // MTU-safe

// Audio stream configuration for protocol negotiation
struct AudioStreamConfig {
    uint16_t port = 9503;
    uint16_t sample_rate = 48000;
    uint8_t channels = 2;
    uint8_t frame_ms = 10;
};

}  // namespace stream_tablet
