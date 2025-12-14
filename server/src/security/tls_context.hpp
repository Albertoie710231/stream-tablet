#pragma once

#include <string>
#include <openssl/ssl.h>

namespace stream_tablet {

class TLSContext {
public:
    TLSContext();
    ~TLSContext();

    // Initialize as server
    bool init_server(const std::string& cert_file, const std::string& key_file,
                     const std::string& ca_file = "");

    // Initialize as client (for testing)
    bool init_client(const std::string& ca_file = "");

    // Get the SSL context
    SSL_CTX* get() const { return m_ctx; }

    // Check if initialized
    bool is_valid() const { return m_ctx != nullptr; }

    void shutdown();

private:
    SSL_CTX* m_ctx = nullptr;
};

}  // namespace stream_tablet
