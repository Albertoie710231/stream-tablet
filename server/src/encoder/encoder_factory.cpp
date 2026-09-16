#include "encoder_factory.hpp"
#include "../util/logger.hpp"

#ifdef HAVE_VAAPI
#include "vaapi_encoder.hpp"
#endif

#ifdef HAVE_CUDA
#include "cuda_encoder.hpp"
#endif

namespace stream_tablet {

std::unique_ptr<EncoderBackend> create_encoder(const EncoderConfig& config) {
    // Try VAAPI first (user preference)
#ifdef HAVE_VAAPI
    {
        LOG_INFO("Attempting VAAPI encoder initialization...");
        auto encoder = std::make_unique<VAAPIEncoder>();
        if (encoder->init(config)) {
            LOG_INFO("Using VAAPI hardware encoder");
            return encoder;
        }
        LOG_WARN("VAAPI initialization failed, trying CUDA...");
    }
#else
    LOG_INFO("VAAPI support not compiled in");
#endif

    // Try CUDA as fallback
#ifdef HAVE_CUDA
    {
        LOG_INFO("Attempting CUDA/NVENC encoder initialization...");
        auto encoder = std::make_unique<CUDAEncoder>();
        if (encoder->init(config)) {
            LOG_INFO("Using CUDA/NVENC hardware encoder");
            return encoder;
        }
        LOG_WARN("CUDA/NVENC initialization failed");
    }
#else
    LOG_INFO("CUDA support not compiled in");
#endif

    LOG_ERROR("No hardware encoder available (tried VAAPI and CUDA)");
    return nullptr;
}

}  // namespace stream_tablet
