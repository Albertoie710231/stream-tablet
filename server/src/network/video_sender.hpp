#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <netinet/in.h>
#include <openssl/ssl.h>

namespace stream_tablet {

enum class PacingMode {
    AUTO,       // Detect based on IP range
    NONE,       // No pacing (lowest latency, may drop packets)
    LIGHT,      // Light pacing for WiFi
    AGGRESSIVE, // Aggressive pacing for USB tethering
    KEYFRAME    // Only pace keyframes (best for high-bandwidth links)
};

class VideoSender {
public:
    VideoSender();
    ~VideoSender();

    // Initialize UDP socket
    bool init(uint16_t port);

    // Set client address (called when client connects)
    // Automatically detects pacing mode based on IP if mode is AUTO
    void set_client(const std::string& host, uint16_t port, PacingMode mode = PacingMode::AUTO);

    // Send encoded frame (fragments if necessary)
    bool send_frame(const uint8_t* data, size_t size,
                    uint32_t frame_number, bool keyframe, uint64_t timestamp_us);

    // Get statistics
    uint64_t get_bytes_sent() const { return m_bytes_sent; }
    uint64_t get_packets_sent() const { return m_packets_sent; }

    void shutdown();

private:
    bool send_packet(const uint8_t* data, size_t size);
    PacingMode detect_pacing_mode(const std::string& host);

    int m_socket = -1;
    struct sockaddr_in m_client_addr = {};
    bool m_client_set = false;

    uint16_t m_sequence = 0;
    uint64_t m_bytes_sent = 0;
    uint64_t m_packets_sent = 0;

    // Pacing configuration
    PacingMode m_pacing_mode = PacingMode::LIGHT;
    size_t m_pacing_threshold = 0;    // Frame size threshold for pacing
    int m_packets_per_burst = 0;      // Packets before pause
    int m_burst_delay_us = 0;         // Microseconds to pause
};

// Video packet header (16 bytes)
#pragma pack(push, 1)
struct VideoPacketHeader {
    uint16_t magic;         // 0x5354 ("ST")
    uint16_t sequence;      // Packet sequence number
    uint16_t frame_number;  // Frame number
    uint8_t flags;          // bit 0: keyframe, bit 1: start of frame, bit 2: end of frame
    uint8_t reserved;
    uint16_t fragment_idx;  // Fragment index (0-65535)
    uint16_t fragment_count;// Total fragments
    uint16_t payload_len;   // Payload length
    uint16_t reserved2;
};
#pragma pack(pop)

static_assert(sizeof(VideoPacketHeader) == 16, "VideoPacketHeader must be 16 bytes");

constexpr uint16_t VIDEO_MAGIC = 0x5354;
constexpr uint8_t FLAG_KEYFRAME = 0x01;
constexpr uint8_t FLAG_START_OF_FRAME = 0x02;
constexpr uint8_t FLAG_END_OF_FRAME = 0x04;

}  // namespace stream_tablet
