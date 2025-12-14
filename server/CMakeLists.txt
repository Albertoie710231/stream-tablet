# Core sources (always built)
set(SERVER_SOURCES
    src/main.cpp
    src/server.cpp
    src/encoder/vaapi_encoder.cpp
    src/network/control_server.cpp
    src/network/video_sender.cpp
    src/network/input_receiver.cpp
    src/input/uinput_backend.cpp
    src/input/coord_transform.cpp
    src/security/tls_context.cpp
    src/util/event_loop.cpp
    src/util/logger.cpp
)

# Conditionally add capture backends
if(HAVE_X11)
    list(APPEND SERVER_SOURCES src/capture/x11_capture.cpp)
endif()

if(HAVE_PIPEWIRE)
    list(APPEND SERVER_SOURCES src/capture/pipewire_capture.cpp)
endif()

add_executable(stream_tablet_server ${SERVER_SOURCES})

# Include directories
target_include_directories(stream_tablet_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VA_INCLUDE_DIRS}
    ${DRM_INCLUDE_DIRS}
    ${UV_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)

if(HAVE_X11)
    target_include_directories(stream_tablet_server PRIVATE ${XCB_INCLUDE_DIRS})
endif()

if(HAVE_PIPEWIRE)
    target_include_directories(stream_tablet_server PRIVATE
        ${PIPEWIRE_INCLUDE_DIRS}
        ${GIO_INCLUDE_DIRS}
    )
endif()

# Link libraries
target_link_libraries(stream_tablet_server PRIVATE
    ${VA_LIBRARIES}
    ${DRM_LIBRARIES}
    ${UV_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(HAVE_X11)
    target_link_libraries(stream_tablet_server PRIVATE ${XCB_LIBRARIES})
endif()

if(HAVE_PIPEWIRE)
    target_link_libraries(stream_tablet_server PRIVATE
        ${PIPEWIRE_LIBRARIES}
        ${GIO_LIBRARIES}
    )
endif()

# Compile definitions for conditional compilation
if(HAVE_X11)
    target_compile_definitions(stream_tablet_server PRIVATE HAVE_X11=1)
endif()

if(HAVE_PIPEWIRE)
    target_compile_definitions(stream_tablet_server PRIVATE HAVE_PIPEWIRE=1)
    # PipeWire/SPA headers use C99 compound literals and take addresses of rvalues
    # which requires -fpermissive in C++ and disabling -Wpedantic
    set_source_files_properties(src/capture/pipewire_capture.cpp
        PROPERTIES COMPILE_FLAGS "-Wno-pedantic -fpermissive")
endif()

target_compile_options(stream_tablet_server PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O3 -march=native -mtune=native -ffast-math>
    $<$<CONFIG:Debug>:-g -O0>
    # Enable SSE2/AVX for SIMD optimizations
    -msse2
)

# Install
install(TARGETS stream_tablet_server RUNTIME DESTINATION bin)
