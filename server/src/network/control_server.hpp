#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <openssl/ssl.h>

namespace stream_tablet {

struct ClientInfo {
    std::string host;
    uint16_t video_port = 0;
    uint16_t input_port = 0;
    int width = 0;
    int height = 0;
};

class ControlServer {
public:
    using ClientConnectCallback = std::function<void(const ClientInfo&)>;
    using ClientDisconnectCallback = std::function<void()>;
    using KeyframeRequestCallback = std::function<void()>;

    ControlServer();
    ~ControlServer();

    // Initialize server with TLS
    bool init(uint16_t port, const std::string& cert_file, const std::string& key_file);

    // Initialize server without TLS (for development)
    bool init_plain(uint16_t port);

    // Accept client connection (blocking)
    bool accept_client(ClientInfo& out_info);

    // Send configuration to client
    bool send_config(int screen_width, int screen_height, int video_port, int input_port);

    // Send configuration with audio info
    bool send_config_with_audio(int screen_width, int screen_height, int video_port, int input_port,
                                 int audio_port, int audio_sample_rate, int audio_channels, int audio_frame_ms);

    // Process incoming messages (call periodically)
    void process();

    // Callbacks
    void set_keyframe_callback(KeyframeRequestCallback cb) { m_keyframe_cb = std::move(cb); }

    // Get client address (for video sender)
    std::string get_client_host() const { return m_client_host; }

    // Check if client is still connected
    bool is_client_connected() const { return m_client_connected; }

    // Reset for new connection
    void reset();

    void shutdown();

private:
    bool init_tls(const std::string& cert_file, const std::string& key_file);
    bool read_message(uint8_t& type, std::vector<uint8_t>& data);
    bool send_message(uint8_t type, const uint8_t* data, size_t len);

    int m_listen_socket = -1;
    int m_client_socket = -1;
    std::string m_client_host;
    bool m_client_connected = false;

    SSL_CTX* m_ssl_ctx = nullptr;
    SSL* m_ssl = nullptr;
    bool m_use_tls = false;

    KeyframeRequestCallback m_keyframe_cb;
};

// Control message types
constexpr uint8_t MSG_AUTH_REQUEST = 0x01;
constexpr uint8_t MSG_AUTH_RESPONSE = 0x02;
constexpr uint8_t MSG_CONFIG_REQUEST = 0x03;
constexpr uint8_t MSG_CONFIG_RESPONSE = 0x04;
constexpr uint8_t MSG_KEYFRAME_REQUEST = 0x05;
constexpr uint8_t MSG_PING = 0x06;
constexpr uint8_t MSG_PONG = 0x07;
constexpr uint8_t MSG_DISCONNECT = 0x08;

}  // namespace stream_tablet
