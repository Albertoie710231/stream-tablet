#include "control_server.hpp"
#include "../util/logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <vector>

namespace stream_tablet {

ControlServer::ControlServer() = default;

ControlServer::~ControlServer() {
    shutdown();
}

bool ControlServer::init_plain(uint16_t port) {
    m_use_tls = false;

    m_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen_socket < 0) {
        LOG_ERROR("Failed to create TCP socket");
        return false;
    }

    int opt = 1;
    setsockopt(m_listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_listen_socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind control socket to port %d", port);
        close(m_listen_socket);
        m_listen_socket = -1;
        return false;
    }

    if (listen(m_listen_socket, 1) < 0) {
        LOG_ERROR("Failed to listen on control socket");
        close(m_listen_socket);
        m_listen_socket = -1;
        return false;
    }

    LOG_INFO("Control server listening on port %d (no TLS)", port);
    return true;
}

bool ControlServer::init(uint16_t port, const std::string& cert_file, const std::string& key_file) {
    if (!init_tls(cert_file, key_file)) {
        LOG_WARN("TLS init failed, falling back to plain TCP");
        return init_plain(port);
    }

    m_use_tls = true;

    m_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen_socket < 0) {
        LOG_ERROR("Failed to create TCP socket");
        return false;
    }

    int opt = 1;
    setsockopt(m_listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_listen_socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind control socket to port %d", port);
        close(m_listen_socket);
        m_listen_socket = -1;
        return false;
    }

    if (listen(m_listen_socket, 1) < 0) {
        LOG_ERROR("Failed to listen on control socket");
        close(m_listen_socket);
        m_listen_socket = -1;
        return false;
    }

    LOG_INFO("Control server listening on port %d (TLS)", port);
    return true;
}

bool ControlServer::init_tls(const std::string& cert_file, const std::string& key_file) {
    SSL_library_init();
    SSL_load_error_strings();

    const SSL_METHOD* method = TLS_server_method();
    m_ssl_ctx = SSL_CTX_new(method);
    if (!m_ssl_ctx) {
        LOG_ERROR("Failed to create SSL context");
        return false;
    }

    if (SSL_CTX_use_certificate_file(m_ssl_ctx, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR("Failed to load certificate: %s", cert_file.c_str());
        SSL_CTX_free(m_ssl_ctx);
        m_ssl_ctx = nullptr;
        return false;
    }

    if (SSL_CTX_use_PrivateKey_file(m_ssl_ctx, key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR("Failed to load private key: %s", key_file.c_str());
        SSL_CTX_free(m_ssl_ctx);
        m_ssl_ctx = nullptr;
        return false;
    }

    LOG_INFO("TLS initialized");
    return true;
}

bool ControlServer::accept_client(ClientInfo& out_info) {
    if (m_listen_socket < 0) {
        return false;
    }

    struct sockaddr_in client_addr = {};
    socklen_t client_len = sizeof(client_addr);

    LOG_INFO("Waiting for client connection...");
    m_client_socket = accept(m_listen_socket, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
    if (m_client_socket < 0) {
        LOG_ERROR("Failed to accept client");
        return false;
    }

    char host[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, host, sizeof(host));
    m_client_host = host;

    LOG_INFO("Client connected from %s", m_client_host.c_str());

    if (m_use_tls && m_ssl_ctx) {
        m_ssl = SSL_new(m_ssl_ctx);
        SSL_set_fd(m_ssl, m_client_socket);

        if (SSL_accept(m_ssl) <= 0) {
            LOG_ERROR("TLS handshake failed");
            SSL_free(m_ssl);
            m_ssl = nullptr;
            close(m_client_socket);
            m_client_socket = -1;
            return false;
        }
        LOG_INFO("TLS handshake completed");
    }

    // Read config request from client
    uint8_t msg_type;
    std::vector<uint8_t> msg_data;
    if (!read_message(msg_type, msg_data) || msg_type != MSG_CONFIG_REQUEST) {
        LOG_ERROR("Expected config request from client");
        return false;
    }

    // Parse config request (simple format: width(2), height(2), video_port(2), input_port(2))
    if (msg_data.size() >= 8) {
        out_info.width = (msg_data[0] << 8) | msg_data[1];
        out_info.height = (msg_data[2] << 8) | msg_data[3];
        out_info.video_port = (msg_data[4] << 8) | msg_data[5];
        out_info.input_port = (msg_data[6] << 8) | msg_data[7];
    }
    out_info.host = m_client_host;

    LOG_INFO("Client config: %dx%d, video_port=%d, input_port=%d",
             out_info.width, out_info.height, out_info.video_port, out_info.input_port);

    m_client_connected = true;
    return true;
}

bool ControlServer::send_config(int screen_width, int screen_height, int video_port, int input_port) {
    std::vector<uint8_t> data(8);
    data[0] = (screen_width >> 8) & 0xFF;
    data[1] = screen_width & 0xFF;
    data[2] = (screen_height >> 8) & 0xFF;
    data[3] = screen_height & 0xFF;
    data[4] = (video_port >> 8) & 0xFF;
    data[5] = video_port & 0xFF;
    data[6] = (input_port >> 8) & 0xFF;
    data[7] = input_port & 0xFF;

    return send_message(MSG_CONFIG_RESPONSE, data.data(), data.size());
}

bool ControlServer::send_config_with_audio(int screen_width, int screen_height, int video_port, int input_port,
                                            int audio_port, int audio_sample_rate, int audio_channels, int audio_frame_ms) {
    // Extended config: 14 bytes
    // [width:2][height:2][video_port:2][input_port:2][audio_port:2][sample_rate:2][channels:1][frame_ms:1]
    std::vector<uint8_t> data(14);
    data[0] = (screen_width >> 8) & 0xFF;
    data[1] = screen_width & 0xFF;
    data[2] = (screen_height >> 8) & 0xFF;
    data[3] = screen_height & 0xFF;
    data[4] = (video_port >> 8) & 0xFF;
    data[5] = video_port & 0xFF;
    data[6] = (input_port >> 8) & 0xFF;
    data[7] = input_port & 0xFF;
    data[8] = (audio_port >> 8) & 0xFF;
    data[9] = audio_port & 0xFF;
    data[10] = (audio_sample_rate >> 8) & 0xFF;
    data[11] = audio_sample_rate & 0xFF;
    data[12] = static_cast<uint8_t>(audio_channels);
    data[13] = static_cast<uint8_t>(audio_frame_ms);

    LOG_INFO("Sending config with audio: %dx%d, video=%d, input=%d, audio=%d, %dHz, %dch, %dms",
             screen_width, screen_height, video_port, input_port, audio_port,
             audio_sample_rate, audio_channels, audio_frame_ms);

    return send_message(MSG_CONFIG_RESPONSE, data.data(), data.size());
}

bool ControlServer::send_config_full(int screen_width, int screen_height, int video_port, int input_port,
                                      int audio_port, int audio_sample_rate, int audio_channels, int audio_frame_ms,
                                      uint8_t codec_type) {
    // Full config: 15 bytes
    // [width:2][height:2][video_port:2][input_port:2][audio_port:2][sample_rate:2][channels:1][frame_ms:1][codec:1]
    std::vector<uint8_t> data(15);
    data[0] = (screen_width >> 8) & 0xFF;
    data[1] = screen_width & 0xFF;
    data[2] = (screen_height >> 8) & 0xFF;
    data[3] = screen_height & 0xFF;
    data[4] = (video_port >> 8) & 0xFF;
    data[5] = video_port & 0xFF;
    data[6] = (input_port >> 8) & 0xFF;
    data[7] = input_port & 0xFF;
    data[8] = (audio_port >> 8) & 0xFF;
    data[9] = audio_port & 0xFF;
    data[10] = (audio_sample_rate >> 8) & 0xFF;
    data[11] = audio_sample_rate & 0xFF;
    data[12] = static_cast<uint8_t>(audio_channels);
    data[13] = static_cast<uint8_t>(audio_frame_ms);
    data[14] = codec_type;

    const char* codec_names[] = {"AV1", "HEVC", "H.264"};
    const char* codec_name = (codec_type < 3) ? codec_names[codec_type] : "unknown";

    LOG_INFO("Sending config: %dx%d, video=%d, input=%d, audio=%d, %dHz, %dch, %dms, codec=%s",
             screen_width, screen_height, video_port, input_port, audio_port,
             audio_sample_rate, audio_channels, audio_frame_ms, codec_name);

    return send_message(MSG_CONFIG_RESPONSE, data.data(), data.size());
}

bool ControlServer::read_message(uint8_t& type, std::vector<uint8_t>& data) {
    // Message format: [length:2][type:1][data:length-1]
    uint8_t header[3];
    ssize_t n;

    if (m_use_tls && m_ssl) {
        n = SSL_read(m_ssl, header, 3);
    } else {
        n = recv(m_client_socket, header, 3, MSG_WAITALL);
    }

    if (n != 3) {
        return false;
    }

    uint16_t length = (header[0] << 8) | header[1];
    type = header[2];

    if (length > 1) {
        data.resize(length - 1);
        if (m_use_tls && m_ssl) {
            n = SSL_read(m_ssl, data.data(), data.size());
        } else {
            n = recv(m_client_socket, data.data(), data.size(), MSG_WAITALL);
        }
        if (n != static_cast<ssize_t>(data.size())) {
            return false;
        }
    } else {
        data.clear();
    }

    return true;
}

bool ControlServer::send_message(uint8_t type, const uint8_t* data, size_t len) {
    std::vector<uint8_t> msg(3 + len);
    uint16_t length = static_cast<uint16_t>(len + 1);
    msg[0] = (length >> 8) & 0xFF;
    msg[1] = length & 0xFF;
    msg[2] = type;
    if (len > 0) {
        memcpy(msg.data() + 3, data, len);
    }

    ssize_t n;
    if (m_use_tls && m_ssl) {
        n = SSL_write(m_ssl, msg.data(), msg.size());
    } else {
        n = send(m_client_socket, msg.data(), msg.size(), 0);
    }

    return n == static_cast<ssize_t>(msg.size());
}

void ControlServer::process() {
    if (m_client_socket < 0 || !m_client_connected) {
        return;
    }

    // Check for incoming messages (non-blocking)
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(m_client_socket, &readfds);

    struct timeval tv = {0, 0};  // Non-blocking
    if (select(m_client_socket + 1, &readfds, nullptr, nullptr, &tv) > 0) {
        uint8_t msg_type;
        std::vector<uint8_t> msg_data;
        if (read_message(msg_type, msg_data)) {
            switch (msg_type) {
                case MSG_KEYFRAME_REQUEST:
                    if (m_keyframe_cb) {
                        m_keyframe_cb();
                    }
                    break;
                case MSG_PING: {
                    // Echo back as pong
                    send_message(MSG_PONG, msg_data.data(), msg_data.size());
                    break;
                }
                case MSG_DISCONNECT:
                    LOG_INFO("Client sent disconnect message");
                    m_client_connected = false;
                    break;
            }
        } else {
            // read_message failed - client disconnected
            LOG_INFO("Client connection lost");
            m_client_connected = false;
        }
    }
}

void ControlServer::reset() {
    // Close client connection but keep listen socket
    if (m_ssl) {
        SSL_shutdown(m_ssl);
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
    if (m_client_socket >= 0) {
        close(m_client_socket);
        m_client_socket = -1;
    }
    m_client_connected = false;
    m_client_host.clear();
}

void ControlServer::shutdown() {
    reset();
    if (m_ssl_ctx) {
        SSL_CTX_free(m_ssl_ctx);
        m_ssl_ctx = nullptr;
    }
    if (m_listen_socket >= 0) {
        close(m_listen_socket);
        m_listen_socket = -1;
    }
}

}  // namespace stream_tablet
