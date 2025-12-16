cmake_minimum_required(VERSION 3.20)
project(stream_tablet VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Options
option(ENABLE_TESTS "Build tests" OFF)
option(ENABLE_X11 "Enable X11 capture backend" ON)
option(ENABLE_PIPEWIRE "Enable PipeWire capture backend" ON)

# Find dependencies
find_package(PkgConfig REQUIRED)

# Core dependencies (always required)
pkg_check_modules(VA REQUIRED libva libva-drm)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(UV REQUIRED libuv)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
find_package(OpenSSL REQUIRED)

# Optional: X11 capture
if(ENABLE_X11)
    pkg_check_modules(XCB xcb xcb-shm xcb-image xcb-xfixes)
    if(XCB_FOUND)
        message(STATUS "X11 capture: enabled")
        set(HAVE_X11 TRUE)
    else()
        message(STATUS "X11 capture: disabled (xcb not found)")
        set(HAVE_X11 FALSE)
    endif()
else()
    message(STATUS "X11 capture: disabled")
    set(HAVE_X11 FALSE)
endif()

# Optional: PipeWire capture
if(ENABLE_PIPEWIRE)
    pkg_check_modules(PIPEWIRE libpipewire-0.3)
    pkg_check_modules(GIO gio-2.0 gio-unix-2.0)
    if(PIPEWIRE_FOUND AND GIO_FOUND)
        message(STATUS "PipeWire capture: enabled")
        set(HAVE_PIPEWIRE TRUE)
    else()
        message(STATUS "PipeWire capture: disabled (libpipewire or gio not found)")
        set(HAVE_PIPEWIRE FALSE)
    endif()
else()
    message(STATUS "PipeWire capture: disabled")
    set(HAVE_PIPEWIRE FALSE)
endif()

# Audio support
option(ENABLE_AUDIO "Enable audio streaming" ON)

if(ENABLE_AUDIO)
    # Opus is required for audio
    pkg_check_modules(OPUS opus)
    if(OPUS_FOUND)
        message(STATUS "Audio (Opus): enabled")
        set(HAVE_OPUS TRUE)
    else()
        message(STATUS "Audio (Opus): disabled (libopus not found)")
        set(HAVE_OPUS FALSE)
    endif()

    # PulseAudio for audio capture fallback
    pkg_check_modules(PULSE libpulse-simple)
    if(PULSE_FOUND)
        message(STATUS "PulseAudio audio: enabled (fallback)")
        set(HAVE_PULSE TRUE)
    else()
        message(STATUS "PulseAudio audio: disabled (libpulse not found)")
        set(HAVE_PULSE FALSE)
    endif()
else()
    message(STATUS "Audio: disabled")
    set(HAVE_OPUS FALSE)
    set(HAVE_PULSE FALSE)
endif()

# Ensure at least one capture backend
if(NOT HAVE_X11 AND NOT HAVE_PIPEWIRE)
    message(FATAL_ERROR "At least one capture backend (X11 or PipeWire) is required")
endif()

# Add server subdirectory
add_subdirectory(server)

if(ENABLE_TESTS)
    enable_testing()
endif()
