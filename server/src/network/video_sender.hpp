#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>
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
    uint64_t get_feedback_packets() const { return m_feedback_packets; }
    uint64_t get_keyframe_requests() const { return m_keyframe_requests_via_udp; }
    uint64_t get_frames_dropped() const { return m_frames_dropped; }

    // Register a callback fired when a NACK / keyframe-request feedback
    // packet arrives on the UDP video socket. Server wires this to
    // m_encoder->request_keyframe(). Safe to call before or after init().
    void set_keyframe_request_callback(std::function<void()> cb);

    // Upper bound on how long send_frame() may spend pacing a single frame.
    // send_frame runs on the capture loop, so pacing time is taken directly
    // out of the frame budget. Set from the negotiated framerate.
    void set_max_pacing_us(long us) { m_max_pacing_us = us; }

    // Fired when the send queue had to discard a frame. Dropping an inter
    // frame breaks the decoder's reference chain, so the caller must force a
    // keyframe or the picture stays corrupt until the next scheduled one.
    void set_frame_dropped_callback(std::function<void()> cb) { m_frame_dropped_cb = std::move(cb); }

    void shutdown();

private:
    bool send_packet(const uint8_t* data, size_t size);
    PacingMode detect_pacing_mode(const std::string& host);
    void feedback_loop();

    // One encoded frame waiting to go out on the wire.
    struct PendingFrame {
        std::vector<uint8_t> data;
        uint32_t frame_number = 0;
        bool keyframe = false;
    };
    void sender_loop();
    void transmit(const PendingFrame& f);

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
    long m_max_pacing_us = 4000;      // Cap on total pacing time per frame

    // Transmission runs on its own thread. Pacing a large keyframe means
    // sleeping between bursts, and doing that on the capture loop costs frames
    // directly — a 485-packet keyframe spread over 70ms is four lost frames at
    // 60fps. Off-thread, the spread is free and the link stops dropping packets.
    std::thread m_sender_thread;
    std::atomic<bool> m_sender_running{false};
    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::deque<PendingFrame> m_queue;
    std::atomic<uint64_t> m_frames_dropped{0};
    std::function<void()> m_frame_dropped_cb;

    // UDP feedback path (drains rx_queue + handles NACK/keyframe-request)
    std::thread m_feedback_thread;
    std::atomic<bool> m_feedback_running{false};
    std::mutex m_feedback_cb_mutex;
    std::function<void()> m_keyframe_request_cb;
    std::atomic<uint64_t> m_feedback_packets{0};
    std::atomic<uint64_t> m_keyframe_requests_via_udp{0};
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

// UDP feedback packet sent client -> server on the video port.
// 4 bytes: magic(2) + type(1) + reserved(1). Anything not matching this
// is silently drained (e.g. legacy 1-byte client init "punch" packets).
constexpr uint16_t FEEDBACK_MAGIC = 0x4246;  // "FB" little-endian
constexpr uint8_t  FEEDBACK_TYPE_KEYFRAME_REQUEST = 0x01;
#pragma pack(push, 1)
struct FeedbackPacket {
    uint16_t magic;
    uint8_t  type;
    uint8_t  reserved;
};
#pragma pack(pop)
static_assert(sizeof(FeedbackPacket) == 4, "FeedbackPacket must be 4 bytes");

}  // namespace stream_tablet
