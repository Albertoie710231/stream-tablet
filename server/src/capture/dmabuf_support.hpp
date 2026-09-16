#pragma once

#include <cstdint>
#include <vector>

namespace stream_tablet {

// DRM format modifiers supported by the render node for a given DRM fourcc.
// Queried once via EGL (eglQueryDmaBufModifiersEXT) on a GBM device.
//
// The list is what we offer KWin during SPA_PARAM_EnumFormat negotiation; KWin
// intersects it with what its own renderer can export and fixates one. If the
// query fails (no EGL, no render node, extension missing) we fall back to
// {DRM_FORMAT_MOD_INVALID}, which asks for an implicit-modifier buffer — still
// importable by VAAPI, just without explicit tiling info.
std::vector<uint64_t> query_supported_modifiers(uint32_t drm_format,
                                                const char* render_node = nullptr);

// Map a SPA_VIDEO_FORMAT_* to a DRM fourcc. Returns 0 if unmapped.
uint32_t spa_format_to_drm_fourcc(uint32_t spa_format);

// Human-readable modifier name for logging (e.g. "INTEL_4_TILED", "LINEAR").
const char* drm_modifier_name(uint64_t modifier);

}  // namespace stream_tablet
