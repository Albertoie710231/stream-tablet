#!/bin/bash

# Install dependencies for StreamTablet server on Arch Linux

set -e

echo "Installing dependencies..."

sudo pacman -S --needed \
    cmake \
    libxcb \
    xcb-util-image \
    libva \
    libva-utils \
    libdrm \
    openssl \
    libuv \
    opus \
    libpulse

echo ""
echo "Dependencies installed successfully!"
echo ""
echo "To build the server:"
echo "  cd stream-tablet"
echo "  mkdir build && cd build"
echo "  cmake .."
echo "  make -j$(nproc)"
