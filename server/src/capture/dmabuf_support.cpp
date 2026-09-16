#include "dmabuf_support.hpp"
#include "../util/logger.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <libdrm/drm_fourcc.h>
#include <spa/param/video/raw.h>

#include <fcntl.h>
#include <unistd.h>

namespace stream_tablet {

uint32_t spa_format_to_drm_fourcc(uint32_t spa_format) {
    // SPA names describe byte order in memory; DRM fourccs are little-endian
    // packed words, so BGRx (bytes B,G,R,x) == DRM_FORMAT_XRGB8888.
    switch (spa_format) {
        case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
        case SPA_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
        case SPA_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
        case SPA_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
        default: return 0;
    }
}

const char* drm_modifier_name(uint64_t modifier) {
    switch (modifier) {
        case DRM_FORMAT_MOD_INVALID:        return "INVALID(implicit)";
        case DRM_FORMAT_MOD_LINEAR:         return "LINEAR";
        case I915_FORMAT_MOD_X_TILED:       return "I915_X_TILED";
        case I915_FORMAT_MOD_Y_TILED:       return "I915_Y_TILED";
#ifdef I915_FORMAT_MOD_4_TILED
        case I915_FORMAT_MOD_4_TILED:       return "I915_4_TILED";
#endif
#ifdef I915_FORMAT_MOD_4_TILED_BMG_CCS
        case I915_FORMAT_MOD_4_TILED_BMG_CCS: return "I915_4_TILED_BMG_CCS";
#endif
        default:                            return "other";
    }
}

std::vector<uint64_t> query_supported_modifiers(uint32_t drm_format,
                                                const char* render_node) {
    std::vector<uint64_t> result;

    const char* node = render_node ? render_node : "/dev/dri/renderD128";
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG_WARN("dmabuf: cannot open %s, falling back to implicit modifier", node);
        return {DRM_FORMAT_MOD_INVALID};
    }

    struct gbm_device* gbm = gbm_create_device(fd);
    if (!gbm) {
        LOG_WARN("dmabuf: gbm_create_device failed, falling back to implicit modifier");
        close(fd);
        return {DRM_FORMAT_MOD_INVALID};
    }

    EGLDisplay dpy = EGL_NO_DISPLAY;
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display) {
        dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
    }

    EGLint major = 0, minor = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        LOG_WARN("dmabuf: EGL init failed on %s, falling back to implicit modifier", node);
        gbm_device_destroy(gbm);
        close(fd);
        return {DRM_FORMAT_MOD_INVALID};
    }

    auto query_modifiers = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
        eglGetProcAddress("eglQueryDmaBufModifiersEXT"));

    EGLint num = 0;
    if (query_modifiers && query_modifiers(dpy, static_cast<EGLint>(drm_format),
                                           0, nullptr, nullptr, &num) && num > 0) {
        std::vector<EGLuint64KHR> mods(num);
        std::vector<EGLBoolean> external(num);
        if (query_modifiers(dpy, static_cast<EGLint>(drm_format), num,
                            mods.data(), external.data(), &num)) {
            for (EGLint i = 0; i < num; i++) {
                // Skip external-only modifiers: VAAPI import wants sampleable
                // buffers and external-only ones can't be bound as plain images.
                if (external[i]) continue;
                result.push_back(static_cast<uint64_t>(mods[i]));
            }
        }
    }

    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(fd);

    // Always offer the implicit modifier last as a safety net — some KWin
    // renderer paths only export implicit-modifier buffers.
    result.push_back(DRM_FORMAT_MOD_INVALID);

    LOG_INFO("dmabuf: %zu modifier(s) supported for fourcc 0x%08x on %s",
             result.size(), drm_format, node);
    return result;
}

}  // namespace stream_tablet
