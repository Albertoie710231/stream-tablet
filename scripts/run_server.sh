#!/bin/bash

# Run StreamTablet server

SCRIPT_DIR="$(dirname "$0")"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
SERVER="$BUILD_DIR/server/stream_tablet_server"

# Check if server is built
if [ ! -f "$SERVER" ]; then
    echo "Server not built. Building now..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake ..
    make -j$(nproc)
fi

# Make sure uinput is accessible
if [ ! -w /dev/uinput ]; then
    echo "Setting up uinput permissions..."
    sudo chmod 666 /dev/uinput
fi

# Run server
echo "Starting StreamTablet server..."
"$SERVER" "$@"
