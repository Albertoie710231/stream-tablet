#pragma once

#include <cstdint>
#include <string>

namespace stream_tablet {

enum class QualityMode {
    LOW_LATENCY,   // CBR, optimized for latency
    BALANCED,      // CBR, balanced quality/latency
    HIGH_QUALITY   // CQP, optimized for quality
};

struct ServerConfig {
    // Display
    std::string display = ":0";
    int capture_fps = 60;

    // Encoding
    int bitrate = 15000000;  // 15 Mbps
    int gop_size = 60;       // Keyframe every 1 second at 60fps
    QualityMode quality_mode = QualityMode::BALANCED;
    int cqp = 20;            // Quality level for CQP mode (lower = better, 1-51)

    // Network
    uint16_t control_port = 9500;
    uint16_t video_port = 9501;
    uint16_t input_port = 9502;

    // Security
    std::string cert_file = "server.crt";
    std::string key_file = "server.key";
    std::string ca_file = "ca.crt";

    // Target resolution (0 = use screen resolution)
    int target_width = 0;
    int target_height = 0;
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
constexpr uint16_t PROTOCOL_MAGIC = 0x5354;  // "ST"
constexpr uint8_t PACKET_TYPE_VIDEO = 0x01;
constexpr uint8_t PACKET_TYPE_KEYFRAME = 0x02;
constexpr uint8_t PACKET_TYPE_CONFIG = 0x03;

constexpr size_t MAX_PACKET_PAYLOAD = 1200;  // MTU-safe

}  // namespace stream_tablet
