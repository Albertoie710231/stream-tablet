#include "audio_sender.hpp"
#include "../util/logger.hpp"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <vector>

namespace stream_tablet {

AudioSender::AudioSender() = default;

AudioSender::~AudioSender() {
    shutdown();
}

bool AudioSender::init(uint16_t port) {
    m_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket < 0) {
        LOG_ERROR("Failed to create audio UDP socket");
        return false;
    }

    // Bind to port
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind audio UDP socket to port %d", port);
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // Set socket buffer size (smaller than video, audio packets are small)
    int buf_size = 256 * 1024;  // 256 KB
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    LOG_INFO("Audio sender initialized on port %d", port);
    return true;
}

void AudioSender::set_client(const std::string& host, uint16_t port) {
    memset(&m_client_addr, 0, sizeof(m_client_addr));
    m_client_addr.sin_family = AF_INET;
    m_client_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &m_client_addr.sin_addr);
    m_client_set = true;

    LOG_INFO("Audio client set to %s:%d", host.c_str(), port);
}

bool AudioSender::send_packet(const uint8_t* data, size_t size,
                               uint32_t sequence, uint64_t timestamp_us) {
    if (!m_client_set || m_socket < 0) {
        return false;
    }

    // Build packet with header
    std::vector<uint8_t> packet(sizeof(AudioPacketHeader) + size);
    AudioPacketHeader* header = reinterpret_cast<AudioPacketHeader*>(packet.data());

    header->magic = AUDIO_MAGIC;
    header->sequence = static_cast<uint16_t>(sequence & 0xFFFF);
    // Convert timestamp to sample units (48kHz = 48 samples per ms)
    // We use lower 32 bits which wraps every ~24 hours at 48kHz
    header->timestamp = static_cast<uint32_t>((timestamp_us * 48) / 1000);
    header->payload_len = static_cast<uint16_t>(size);
    header->reserved = 0;

    // Copy payload
    memcpy(packet.data() + sizeof(AudioPacketHeader), data, size);

    // Send
    ssize_t sent = sendto(m_socket, packet.data(), packet.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&m_client_addr),
                          sizeof(m_client_addr));
    if (sent < 0) {
        LOG_ERROR("Failed to send audio packet");
        return false;
    }

    m_bytes_sent += sent;
    m_packets_sent++;
    return true;
}

void AudioSender::shutdown() {
    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
        LOG_INFO("Audio sender shut down");
    }
    m_client_set = false;
}

}  // namespace stream_tablet
