#pragma once

#include <cstdint>
#include <sys/types.h>

namespace stream_tablet {

// Publishes the server on the local network via mDNS so the Android app can
// discover it without the user typing an IP address. Implemented as a fork
// of avahi-publish-service — no direct libavahi dependency, and Linux
// PR_SET_PDEATHSIG ensures the child dies if the server crashes.
class MdnsPublisher {
public:
    MdnsPublisher() = default;
    ~MdnsPublisher();

    // Service type is hard-coded to `_stream-tablet._tcp`.
    bool start(uint16_t control_port);
    void stop();

    bool is_running() const { return m_pid > 0; }

private:
    pid_t m_pid = -1;
};

}  // namespace stream_tablet
