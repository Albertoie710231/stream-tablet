#include "video_sender.hpp"
#include "../util/logger.hpp"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>

namespace stream_tablet {

constexpr size_t MAX_PAYLOAD_SIZE = 1200;  // MTU safe

VideoSender::VideoSender() = default;

VideoSender::~VideoSender() {
    shutdown();
}

bool VideoSender::init(uint16_t port) {
    m_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket < 0) {
        LOG_ERROR("Failed to create UDP socket");
        return false;
    }

    // Bind to port
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind UDP socket to port %d", port);
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // Set socket buffer size for better throughput
    int buf_size = 4 * 1024 * 1024;  // 4 MB
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    LOG_INFO("Video sender initialized on port %d", port);
    return true;
}

PacingMode VideoSender::detect_pacing_mode(const std::string& host) {
    // USB tethering typically uses these IP ranges:
    // - 192.168.42.x (Android default USB tethering)
    // - 192.168.43.x (Android WiFi hotspot, but treat same)
    // - 10.x.x.x (some carriers/configurations)

    // Parse first octet
    int first_octet = 0;
    int second_octet = 0;
    if (sscanf(host.c_str(), "%d.%d", &first_octet, &second_octet) >= 1) {
        // 10.x.x.x range - likely USB tethering or cellular
        if (first_octet == 10) {
            LOG_INFO("Detected USB/cellular network (10.x.x.x), using aggressive pacing");
            return PacingMode::AGGRESSIVE;
        }
        // 192.168.42.x or 192.168.43.x - Android USB tethering / hotspot
        if (first_octet == 192 && second_octet == 168) {
            int third_octet = 0;
            if (sscanf(host.c_str(), "192.168.%d", &third_octet) == 1) {
                if (third_octet == 42 || third_octet == 43) {
                    LOG_INFO("Detected Android tethering (192.168.%d.x), using aggressive pacing", third_octet);
                    return PacingMode::AGGRESSIVE;
                }
            }
        }
    }

    LOG_INFO("Detected WiFi network, using light pacing");
    return PacingMode::LIGHT;
}

void VideoSender::set_client(const std::string& host, uint16_t port, PacingMode mode) {
    memset(&m_client_addr, 0, sizeof(m_client_addr));
    m_client_addr.sin_family = AF_INET;
    m_client_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &m_client_addr.sin_addr);
    m_client_set = true;

    // Set pacing mode
    if (mode == PacingMode::AUTO) {
        m_pacing_mode = detect_pacing_mode(host);
    } else {
        m_pacing_mode = mode;
    }

    // Configure pacing parameters based on mode
    switch (m_pacing_mode) {
        case PacingMode::NONE:
            m_pacing_threshold = 1000000000;  // Never pace (1GB threshold)
            m_packets_per_burst = 0;
            m_burst_delay_us = 0;
            LOG_INFO("Pacing: NONE");
            break;
        case PacingMode::LIGHT:
            m_pacing_threshold = 50000;     // Only pace large frames (>50KB)
            m_packets_per_burst = 20;
            m_burst_delay_us = 50;
            LOG_INFO("Pacing: LIGHT (threshold=50KB, burst=20, delay=50us)");
            break;
        case PacingMode::AGGRESSIVE:
            m_pacing_threshold = 2400;      // Pace any frame > 2 packets
            m_packets_per_burst = 4;
            m_burst_delay_us = 200;
            LOG_INFO("Pacing: AGGRESSIVE (threshold=2.4KB, burst=4, delay=200us)");
            break;
        case PacingMode::KEYFRAME:
            // Special mode: only pace keyframes, with very aggressive pacing
            m_pacing_threshold = 0;         // Will check keyframe flag instead
            m_packets_per_burst = 8;        // Small bursts
            m_burst_delay_us = 100;         // 100us between bursts
            LOG_INFO("Pacing: KEYFRAME (keyframes only, burst=8, delay=100us)");
            break;
        default:
            m_pacing_threshold = 50000;
            m_packets_per_burst = 20;
            m_burst_delay_us = 50;
            break;
    }

    LOG_INFO("Video client set to %s:%d", host.c_str(), port);
}

bool VideoSender::send_frame(const uint8_t* data, size_t size,
                             uint32_t frame_number, bool keyframe, uint64_t timestamp_us) {
    if (!m_client_set || m_socket < 0) {
        return false;
    }

    // Calculate number of fragments needed
    size_t num_fragments = (size + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;
    if (num_fragments > 65535) {
        LOG_ERROR("Frame too large: %zu bytes requires %zu fragments", size, num_fragments);
        return false;
    }

    // Log frame sizes for diagnostics
    if (keyframe) {
        LOG_INFO("Keyframe %u: %zu bytes (%zu packets)", frame_number, size, num_fragments);
    }

    // Determine pacing parameters based on mode and frame size
    bool need_pacing = false;
    int packets_per_burst = m_packets_per_burst;
    int burst_delay_us = m_burst_delay_us;

    if (m_pacing_mode == PacingMode::KEYFRAME) {
        // Only pace keyframes, with adaptive pacing based on size
        need_pacing = keyframe;
        if (keyframe && size > 100000) {
            // Adaptive pacing for large keyframes:
            // - Small keyframes (<100KB): no pacing needed
            // - Medium (100-300KB): light pacing (6 packets, 150us)
            // - Large (300-500KB): moderate pacing (4 packets, 200us)
            // - Very large (>500KB): aggressive pacing (2 packets, 300us)
            if (size > 500000) {
                packets_per_burst = 2;
                burst_delay_us = 300;
            } else if (size > 300000) {
                packets_per_burst = 4;
                burst_delay_us = 200;
            } else {
                packets_per_burst = 6;
                burst_delay_us = 150;
            }
        }
    } else if (m_pacing_mode != PacingMode::NONE) {
        // Size-based pacing
        need_pacing = (size > m_pacing_threshold);
    }

    // Send each fragment (with pacing based on mode)
    size_t offset = 0;
    int packets_in_burst = 0;

    for (size_t i = 0; i < num_fragments; i++) {
        size_t payload_size = std::min(MAX_PAYLOAD_SIZE, size - offset);

        // Build packet
        std::vector<uint8_t> packet(sizeof(VideoPacketHeader) + payload_size);
        VideoPacketHeader* header = reinterpret_cast<VideoPacketHeader*>(packet.data());

        header->magic = VIDEO_MAGIC;
        header->sequence = m_sequence++;
        header->frame_number = static_cast<uint16_t>(frame_number & 0xFFFF);
        header->flags = 0;
        if (keyframe) header->flags |= FLAG_KEYFRAME;
        if (i == 0) header->flags |= FLAG_START_OF_FRAME;
        if (i == num_fragments - 1) header->flags |= FLAG_END_OF_FRAME;
        header->fragment_idx = static_cast<uint16_t>(i);
        header->fragment_count = static_cast<uint16_t>(num_fragments);
        header->reserved = 0;
        header->payload_len = static_cast<uint16_t>(payload_size);
        header->reserved2 = 0;

        // Copy payload
        memcpy(packet.data() + sizeof(VideoPacketHeader), data + offset, payload_size);

        // Send
        if (!send_packet(packet.data(), packet.size())) {
            return false;
        }

        offset += payload_size;

        // Pacing to prevent buffer overflow
        if (need_pacing) {
            packets_in_burst++;
            if (packets_in_burst >= packets_per_burst && i < num_fragments - 1) {
                std::this_thread::sleep_for(std::chrono::microseconds(burst_delay_us));
                packets_in_burst = 0;
            }
        }
    }

    return true;
}

bool VideoSender::send_packet(const uint8_t* data, size_t size) {
    ssize_t sent = sendto(m_socket, data, size, 0,
                          reinterpret_cast<struct sockaddr*>(&m_client_addr),
                          sizeof(m_client_addr));
    if (sent < 0) {
        LOG_ERROR("Failed to send packet");
        return false;
    }

    m_bytes_sent += sent;
    m_packets_sent++;
    return true;
}

void VideoSender::shutdown() {
    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
    }
    m_client_set = false;
}

}  // namespace stream_tablet
