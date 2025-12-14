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

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED xcb xcb-shm xcb-image xcb-xfixes)
pkg_check_modules(VA REQUIRED libva libva-drm)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(UV REQUIRED libuv)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
find_package(OpenSSL REQUIRED)

# Add server subdirectory
add_subdirectory(server)

if(ENABLE_TESTS)
    enable_testing()
endif()
