#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <openssl/ssl.h>

namespace stream_tablet {

// Input event types
enum class InputEventType : uint8_t {
    TOUCH_DOWN = 0x01,
    TOUCH_MOVE = 0x02,
    TOUCH_UP = 0x03,
    STYLUS_DOWN = 0x04,
    STYLUS_MOVE = 0x05,
    STYLUS_UP = 0x06,
    STYLUS_HOVER = 0x07,
    KEY_DOWN = 0x08,
    KEY_UP = 0x09
};

struct InputEvent {
    InputEventType type;
    uint8_t pointer_id;
    float x;          // Normalized 0-1
    float y;          // Normalized 0-1
    float pressure;   // 0-1
    float tilt_x;     // Radians
    float tilt_y;     // Radians (orientation)
    uint16_t buttons; // Button state bitfield
    uint32_t timestamp_ms;
};

class InputReceiver {
public:
    using InputCallback = std::function<void(const InputEvent&)>;

    InputReceiver();
    ~InputReceiver();

    // Initialize TCP listener
    bool init(uint16_t port);

    // Accept client connection
    bool accept_client();

    // Set callback for received events
    void set_callback(InputCallback cb) { m_callback = std::move(cb); }

    // Process incoming events (call from event loop)
    void process();

    // Check if connected
    bool is_connected() const { return m_client_socket >= 0; }

    // Reset for new connection
    void reset();

    void shutdown();

private:
    bool read_event(InputEvent& event);

    int m_listen_socket = -1;
    int m_client_socket = -1;

    InputCallback m_callback;
};

// Input event binary format (28 bytes)
#pragma pack(push, 1)
struct InputEventPacket {
    uint8_t type;        // InputEventType
    uint8_t pointer_id;
    float x;             // 4 bytes
    float y;             // 4 bytes
    float pressure;      // 4 bytes
    float tilt_x;        // 4 bytes
    float tilt_y;        // 4 bytes
    uint16_t buttons;    // 2 bytes
    uint32_t timestamp;  // 4 bytes
};
#pragma pack(pop)

static_assert(sizeof(InputEventPacket) == 28, "InputEventPacket must be 28 bytes");

}  // namespace stream_tablet
