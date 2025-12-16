#pragma once

#include <cstdint>
#include <string>
#include <netinet/in.h>

namespace stream_tablet {

class AudioSender {
public:
    AudioSender();
    ~AudioSender();

    // Initialize UDP socket on the given port
    bool init(uint16_t port);

    // Set client address (called when client connects)
    void set_client(const std::string& host, uint16_t port);

    // Send encoded Opus packet
    bool send_packet(const uint8_t* data, size_t size,
                     uint32_t sequence, uint64_t timestamp_us);

    // Get statistics
    uint64_t get_bytes_sent() const { return m_bytes_sent; }
    uint64_t get_packets_sent() const { return m_packets_sent; }

    // Check if client is set
    bool has_client() const { return m_client_set; }

    void shutdown();

private:
    int m_socket = -1;
    struct sockaddr_in m_client_addr = {};
    bool m_client_set = false;

    uint64_t m_bytes_sent = 0;
    uint64_t m_packets_sent = 0;
};

// Audio packet header (12 bytes)
// Simpler than video header - no fragmentation needed for Opus frames
#pragma pack(push, 1)
struct AudioPacketHeader {
    uint16_t magic;         // 0x5341 ("SA" for Stream Audio)
    uint16_t sequence;      // Packet sequence number
    uint32_t timestamp;     // Timestamp in sample units (wraps at 32 bits)
    uint16_t payload_len;   // Payload length in bytes
    uint16_t reserved;      // Reserved for future use
};
#pragma pack(pop)

static_assert(sizeof(AudioPacketHeader) == 12, "AudioPacketHeader must be 12 bytes");

constexpr uint16_t AUDIO_MAGIC = 0x5341;  // "SA"

}  // namespace stream_tablet
