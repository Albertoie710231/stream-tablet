# StreamTablet

Turn your Android tablet into a second screen for your Linux desktop with hardware-accelerated video streaming and full stylus support.

## Features

- **Hardware-accelerated AV1 encoding** using Intel Arc B580 VA-API
- **Low-latency streaming** (~30-40ms on local WiFi)
- **Full stylus support** with pressure and tilt sensitivity
- **Multi-touch support** for touch input
- **X11 screen mirroring**

## Requirements

### Server (Linux)
- Intel Arc B580 GPU (or other VA-API capable GPU)
- X11 display server
- Arch Linux (or compatible distro)

### Client (Android)
- Android 10+ with AV1 hardware decoding support
- Tested on Samsung Galaxy Tab S10+

## Quick Start

### 1. Install Dependencies (Server)

```bash
cd scripts
./install_deps.sh
```

### 2. Build Server

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 3. Run Server

```bash
# Make sure uinput is accessible (for stylus input)
sudo chmod 666 /dev/uinput

# Run server
./server/stream_tablet_server
```

Or use the convenience script:
```bash
./scripts/run_server.sh
```

### 4. Build Android App

1. Open `android/` folder in Android Studio
2. Build and install on your tablet
3. Enter server IP address and connect

## Server Options

```
Usage: stream_tablet_server [options]
Options:
  -d, --display DISPLAY   X11 display (default: :0)
  -f, --fps FPS           Capture FPS (default: 60)
  -b, --bitrate BPS       Bitrate in bps (default: 15000000)
  -p, --port PORT         Control port (default: 9500)
  -v, --verbose           Enable debug logging
  -h, --help              Show this help
```

## Network Ports

- **9500**: Control channel (TCP)
- **9501**: Video stream (UDP)
- **9502**: Input events (TCP)

Make sure these ports are open in your firewall.

## Architecture

```
┌─────────────────────┐                    ┌─────────────────────┐
│   Linux Server      │                    │   Android Client    │
│   (Intel Arc B580)  │                    │   (Tablet)          │
├─────────────────────┤                    ├─────────────────────┤
│ X11 Capture (XCB)   │───Video (AV1)────→ │ MediaCodec Decoder  │
│ VA-API Encoder      │   UDP              │ SurfaceView         │
│                     │                    │                     │
│ uinput Injector     │←──Input ───────────│ Stylus/Touch Handler│
│ (stylus pressure)   │   TCP              │                     │
└─────────────────────┘                    └─────────────────────┘
```

## Troubleshooting

### No video
- Check firewall ports (9500-9502)
- Verify VA-API is working: `vainfo`
- Check server logs with `-v` flag

### Stylus not working
- Run `sudo chmod 666 /dev/uinput`
- Or add yourself to `input` group

### High latency
- Use local WiFi (not through internet)
- Reduce bitrate: `-b 10000000`
- Check network congestion

## License

MIT License
