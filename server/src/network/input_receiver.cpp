#include "input_receiver.hpp"
#include "../util/logger.hpp"
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <fcntl.h>

namespace stream_tablet {

InputReceiver::InputReceiver() = default;

InputReceiver::~InputReceiver() {
    shutdown();
}

bool InputReceiver::init(uint16_t port) {
    m_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen_socket < 0) {
        LOG_ERROR("Failed to create input socket");
        return false;
    }

    int opt = 1;
    setsockopt(m_listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_listen_socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind input socket to port %d", port);
        close(m_listen_socket);
        m_listen_socket = -1;
        return false;
    }

    if (listen(m_listen_socket, 1) < 0) {
        LOG_ERROR("Failed to listen on input socket");
        close(m_listen_socket);
        m_listen_socket = -1;
        return false;
    }

    LOG_INFO("Input receiver listening on port %d", port);
    return true;
}

bool InputReceiver::accept_client() {
    if (m_listen_socket < 0) {
        return false;
    }

    struct sockaddr_in client_addr = {};
    socklen_t client_len = sizeof(client_addr);

    m_client_socket = accept(m_listen_socket, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
    if (m_client_socket < 0) {
        LOG_ERROR("Failed to accept input client");
        return false;
    }

    // Enable TCP_NODELAY for low latency
    int opt = 1;
    setsockopt(m_client_socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Set non-blocking
    int flags = fcntl(m_client_socket, F_GETFL, 0);
    fcntl(m_client_socket, F_SETFL, flags | O_NONBLOCK);

    char host[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, host, sizeof(host));
    LOG_INFO("Input client connected from %s", host);

    return true;
}

bool InputReceiver::read_event(InputEvent& event) {
    constexpr size_t kPacketSize = sizeof(InputEventPacket);

    // Top the buffer up with whatever is available, then parse whole packets
    // out of it. Anything left over stays for the next call.
    if (m_rx_buffer.size() < kPacketSize) {
        uint8_t chunk[4096];
        ssize_t n = recv(m_client_socket, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n > 0) {
            m_rx_buffer.insert(m_rx_buffer.end(), chunk, chunk + n);
        } else if (n == 0) {
            LOG_INFO("Input client disconnected");
            close(m_client_socket);
            m_client_socket = -1;
            m_rx_buffer.clear();
            return false;
        }
        // n < 0 with EAGAIN just means nothing more is available right now.
    }

    if (m_rx_buffer.size() < kPacketSize) {
        return false;
    }

    InputEventPacket packet;
    memcpy(&packet, m_rx_buffer.data(), kPacketSize);
    m_rx_buffer.erase(m_rx_buffer.begin(), m_rx_buffer.begin() + kPacketSize);

    event.type = static_cast<InputEventType>(packet.type);
    event.pointer_id = packet.pointer_id;
    event.x = packet.x;
    event.y = packet.y;
    event.pressure = packet.pressure;
    event.tilt_x = packet.tilt_x;
    event.tilt_y = packet.tilt_y;
    event.buttons = packet.buttons;
    event.timestamp_ms = packet.timestamp;
    return true;
}


void InputReceiver::process() {
    // Try to accept new client if not connected
    if (m_client_socket < 0 && m_listen_socket >= 0) {
        // Non-blocking check for new connection
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(m_listen_socket, &readfds);
        struct timeval tv = {0, 0};
        if (select(m_listen_socket + 1, &readfds, nullptr, nullptr, &tv) > 0) {
            accept_client();
        }
        return;
    }

    InputEvent event;
    while (read_event(event)) {
        if (m_callback) {
            m_callback(event);
        }
    }
}

void InputReceiver::reset() {
    m_rx_buffer.clear();
    if (m_client_socket >= 0) {
        close(m_client_socket);
        m_client_socket = -1;
    }
}

void InputReceiver::shutdown() {
    reset();
    if (m_listen_socket >= 0) {
        close(m_listen_socket);
        m_listen_socket = -1;
    }
}

}  // namespace stream_tablet
