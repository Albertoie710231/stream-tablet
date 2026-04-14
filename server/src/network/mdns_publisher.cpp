#include "network/mdns_publisher.hpp"
#include "util/logger.hpp"

#include <sys/prctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <string>
#include <cstdio>
#include <limits.h>

namespace stream_tablet {

namespace {

std::string host_service_name() {
    char host[HOST_NAME_MAX + 1] = {};
    if (gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0') {
        return "StreamTablet";
    }
    return std::string("StreamTablet @ ") + host;
}

}  // namespace

MdnsPublisher::~MdnsPublisher() {
    stop();
}

bool MdnsPublisher::start(uint16_t control_port) {
    if (m_pid > 0) return true;

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("mdns: fork failed");
        return false;
    }

    if (pid == 0) {
        // Child — die when the parent does, even on SIGKILL.
        prctl(PR_SET_PDEATHSIG, SIGTERM);

        std::string name = host_service_name();
        std::string port = std::to_string(control_port);

        execlp("avahi-publish-service",
               "avahi-publish-service",
               "-s",
               name.c_str(),
               "_stream-tablet._tcp",
               port.c_str(),
               "version=1",
               (char*)nullptr);

        // Exec only returns on failure.
        fprintf(stderr, "mdns: failed to exec avahi-publish-service — "
                        "is avahi-utils installed?\n");
        _exit(127);
    }

    m_pid = pid;
    LOG_INFO("mdns: published _stream-tablet._tcp on port %u (pid %d)",
             control_port, (int)pid);
    return true;
}

void MdnsPublisher::stop() {
    if (m_pid <= 0) return;

    kill(m_pid, SIGTERM);
    int status = 0;
    waitpid(m_pid, &status, 0);
    LOG_INFO("mdns: publisher stopped");
    m_pid = -1;
}

}  // namespace stream_tablet
