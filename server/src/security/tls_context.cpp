#include "tls_context.hpp"
#include "../util/logger.hpp"

namespace stream_tablet {

TLSContext::TLSContext() = default;

TLSContext::~TLSContext() {
    shutdown();
}

bool TLSContext::init_server(const std::string& cert_file, const std::string& key_file,
                             const std::string& ca_file) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    const SSL_METHOD* method = TLS_server_method();
    m_ctx = SSL_CTX_new(method);
    if (!m_ctx) {
        LOG_ERROR("Failed to create SSL context");
        return false;
    }

    // Set minimum TLS version to 1.3
    SSL_CTX_set_min_proto_version(m_ctx, TLS1_3_VERSION);

    // Load certificate
    if (SSL_CTX_use_certificate_file(m_ctx, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR("Failed to load certificate: %s", cert_file.c_str());
        shutdown();
        return false;
    }

    // Load private key
    if (SSL_CTX_use_PrivateKey_file(m_ctx, key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR("Failed to load private key: %s", key_file.c_str());
        shutdown();
        return false;
    }

    // Verify private key
    if (!SSL_CTX_check_private_key(m_ctx)) {
        LOG_ERROR("Private key doesn't match certificate");
        shutdown();
        return false;
    }

    // Load CA for client verification (optional)
    if (!ca_file.empty()) {
        if (SSL_CTX_load_verify_locations(m_ctx, ca_file.c_str(), nullptr) <= 0) {
            LOG_WARN("Failed to load CA file: %s", ca_file.c_str());
        } else {
            // Enable client certificate verification
            SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
            LOG_INFO("Client certificate verification enabled");
        }
    }

    LOG_INFO("TLS server context initialized");
    return true;
}

bool TLSContext::init_client(const std::string& ca_file) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    const SSL_METHOD* method = TLS_client_method();
    m_ctx = SSL_CTX_new(method);
    if (!m_ctx) {
        LOG_ERROR("Failed to create SSL context");
        return false;
    }

    SSL_CTX_set_min_proto_version(m_ctx, TLS1_3_VERSION);

    if (!ca_file.empty()) {
        if (SSL_CTX_load_verify_locations(m_ctx, ca_file.c_str(), nullptr) <= 0) {
            LOG_WARN("Failed to load CA file: %s", ca_file.c_str());
        }
    }

    LOG_INFO("TLS client context initialized");
    return true;
}

void TLSContext::shutdown() {
    if (m_ctx) {
        SSL_CTX_free(m_ctx);
        m_ctx = nullptr;
    }
}

}  // namespace stream_tablet
