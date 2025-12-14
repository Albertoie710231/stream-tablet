add_executable(stream_tablet_server
    src/main.cpp
    src/server.cpp
    src/capture/x11_capture.cpp
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

target_include_directories(stream_tablet_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${XCB_INCLUDE_DIRS}
    ${VA_INCLUDE_DIRS}
    ${DRM_INCLUDE_DIRS}
    ${UV_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)

target_link_libraries(stream_tablet_server PRIVATE
    ${XCB_LIBRARIES}
    ${VA_LIBRARIES}
    ${DRM_LIBRARIES}
    ${UV_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_compile_options(stream_tablet_server PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O3 -march=native -mtune=native -ffast-math>
    $<$<CONFIG:Debug>:-g -O0>
    # Enable SSE2/AVX for SIMD optimizations
    -msse2
)

# Install
install(TARGETS stream_tablet_server RUNTIME DESTINATION bin)
