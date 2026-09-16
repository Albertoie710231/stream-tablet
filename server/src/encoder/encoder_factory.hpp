#pragma once

#include "encoder_backend.hpp"
#include <memory>

namespace stream_tablet {

// Create an encoder with automatic backend selection
// Tries VAAPI first, falls back to CUDA if VAAPI fails
std::unique_ptr<EncoderBackend> create_encoder(const EncoderConfig& config);

}  // namespace stream_tablet
