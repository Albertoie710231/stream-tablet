#!/usr/bin/env python3
"""
CPU Monitor - On-Device Collection (Zero Overhead)
Runs monitoring script on the Android device itself, then pulls data for graphing.

Usage:
    python cpu_monitor_ondevice.py [duration_seconds] [output_file]
"""

import subprocess
import sys
import time
from datetime import datetime, timedelta

try:
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
except ImportError:
    print("matplotlib not found. Install with: pip install matplotlib")
    sys.exit(1)

PACKAGE_NAME = "com.streamtablet"
SAMPLE_INTERVAL = 2
DEVICE_SCRIPT = "/data/local/tmp/cpu_monitor.sh"
DEVICE_OUTPUT = "/data/local/tmp/cpu_data.csv"


def create_device_script(duration, interval):
    """Create a shell script to run on the device - pure /proc, no external commands."""
    script = f'''#!/system/bin/sh
# CPU Monitor - pure /proc reads, minimal overhead
DURATION={duration}
INTERVAL={interval}
PKG="{PACKAGE_NAME}"

PID=$(pidof $PKG)
if [ -z "$PID" ]; then
    echo "App not running"
    exit 1
fi

echo "timestamp,proc_utime,proc_stime,sys_total,sys_idle" > {DEVICE_OUTPUT}

END=$(($(date +%s) + DURATION))

while [ $(date +%s) -lt $END ]; do
    if [ -f /proc/$PID/stat ]; then
        # Read process CPU time (fields 14=utime, 15=stime)
        read -r PSTAT < /proc/$PID/stat
        UTIME=$(echo "$PSTAT" | cut -d' ' -f14)
        STIME=$(echo "$PSTAT" | cut -d' ' -f15)

        # Read system CPU time
        read -r CSTAT < /proc/stat
        set -- $CSTAT
        shift  # Remove "cpu" label
        SYS_TOTAL=$(($1 + $2 + $3 + $4 + $5 + $6 + $7))
        SYS_IDLE=$4

        TS=$(date +%H:%M:%S)
        echo "$TS,$UTIME,$STIME,$SYS_TOTAL,$SYS_IDLE"
        echo "$TS,$UTIME,$STIME,$SYS_TOTAL,$SYS_IDLE" >> {DEVICE_OUTPUT}
    else
        echo "Process died"
        break
    fi

    sleep $INTERVAL
done

echo "Done"
'''
    return script


def run_monitor(duration, output_file):
    """Run the monitor on device and generate graph."""
    print(f"=== On-Device CPU Monitor ===")
    print(f"Duration: {duration}s, Interval: {SAMPLE_INTERVAL}s")
    print(f"This method has ZERO monitoring overhead on the tablet.\n")

    # Check if app is running
    result = subprocess.run(["adb", "shell", f"pidof {PACKAGE_NAME}"],
                          capture_output=True, text=True)
    if not result.stdout.strip():
        print(f"Error: {PACKAGE_NAME} is not running")
        return

    print(f"Found app PID: {result.stdout.strip()}")

    # Create and push script
    script = create_device_script(duration, SAMPLE_INTERVAL)

    # Write script locally first
    with open("/tmp/cpu_monitor.sh", "w") as f:
        f.write(script)

    # Push to device
    subprocess.run(["adb", "push", "/tmp/cpu_monitor.sh", DEVICE_SCRIPT],
                  capture_output=True)
    subprocess.run(["adb", "shell", f"chmod +x {DEVICE_SCRIPT}"],
                  capture_output=True)

    print(f"Running monitor on device for {duration} seconds...")
    print("(You can use the tablet normally - no ADB overhead during collection)\n")

    # Run script on device
    process = subprocess.Popen(
        ["adb", "shell", DEVICE_SCRIPT],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    # Stream output
    try:
        for line in process.stdout:
            print(line.strip())
    except KeyboardInterrupt:
        print("\nStopping...")
        subprocess.run(["adb", "shell", "pkill", "-f", "cpu_monitor.sh"])

    process.wait()

    # Pull data file
    print("\nPulling data from device...")
    subprocess.run(["adb", "pull", DEVICE_OUTPUT, "/tmp/cpu_data.csv"],
                  capture_output=True)

    # Parse and graph
    generate_graph("/tmp/cpu_data.csv", output_file)


def generate_graph(csv_file, output_file):
    """Generate graph from collected data - calculates CPU% from /proc deltas."""
    timestamps = []
    cpu_data = []

    prev_proc_time = None
    prev_sys_total = None
    NUM_CORES = 8  # Adjust if different

    try:
        with open(csv_file, 'r') as f:
            next(f)  # Skip header
            for line in f:
                parts = line.strip().split(',')
                if len(parts) >= 5:
                    try:
                        ts = datetime.strptime(parts[0], '%H:%M:%S')
                        ts = ts.replace(year=datetime.now().year,
                                       month=datetime.now().month,
                                       day=datetime.now().day)

                        proc_utime = int(parts[1])
                        proc_stime = int(parts[2])
                        sys_total = int(parts[3])
                        proc_time = proc_utime + proc_stime

                        if prev_proc_time is not None and prev_sys_total is not None:
                            proc_delta = proc_time - prev_proc_time
                            sys_delta = sys_total - prev_sys_total

                            if sys_delta > 0:
                                # CPU% = (process_delta / system_delta) * 100 * num_cores
                                cpu_pct = (proc_delta / sys_delta) * 100 * NUM_CORES
                                timestamps.append(ts)
                                cpu_data.append(cpu_pct)

                        prev_proc_time = proc_time
                        prev_sys_total = sys_total

                    except (ValueError, IndexError):
                        continue
    except FileNotFoundError:
        print(f"No data file found: {csv_file}")
        return

    if not timestamps:
        print("No data collected!")
        return

    print(f"Collected {len(timestamps)} samples")

    # Create graph
    fig, ax = plt.subplots(figsize=(12, 6))

    ax.plot(timestamps, cpu_data, 'b-', linewidth=2, label='App CPU %')
    ax.fill_between(timestamps, cpu_data, alpha=0.3)

    # Mark spikes
    avg_cpu = sum(cpu_data) / len(cpu_data)
    spike_threshold = avg_cpu * 1.5  # 50% above average
    for t, cpu in zip(timestamps, cpu_data):
        if cpu > spike_threshold:
            ax.axvline(x=t, color='red', alpha=0.2, linestyle='--')

    ax.set_xlabel('Time')
    ax.set_ylabel('CPU Usage (%)')
    ax.set_title('StreamTablet CPU Usage (On-Device - Minimal Overhead)')
    ax.legend()
    ax.grid(True, alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S'))
    plt.xticks(rotation=45)

    plt.tight_layout()
    plt.savefig(output_file, dpi=150)
    print(f"Graph saved to: {output_file}")

    # Stats
    if cpu_data:
        print(f"\n=== Statistics ===")
        print(f"Avg: {sum(cpu_data)/len(cpu_data):.1f}%")
        print(f"Max: {max(cpu_data):.1f}%")
        print(f"Min: {min(cpu_data):.1f}%")
        spikes = [c for c in cpu_data if c > spike_threshold]
        print(f"Spikes (>{spike_threshold:.0f}%): {len(spikes)}")


if __name__ == "__main__":
    duration = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    output = sys.argv[2] if len(sys.argv) > 2 else "cpu_ondevice.png"

    run_monitor(duration, output)
