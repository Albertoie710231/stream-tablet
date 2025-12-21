#!/usr/bin/env python3
"""
CPU Monitor for StreamTablet Android App
Monitors thread-level CPU usage and generates a time-series graph.

Usage:
    python cpu_monitor.py [duration_seconds] [interval_ms] [output_file]

Example:
    python cpu_monitor.py 60 500 cpu_graph.png   # 60s duration, 500ms interval
    python cpu_monitor.py 30 100 fast_sample.png # 30s duration, 100ms interval
"""

import subprocess
import re
import time
import sys
from collections import defaultdict
from datetime import datetime

try:
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
except ImportError:
    print("matplotlib not found. Install with: pip install matplotlib")
    sys.exit(1)

PACKAGE_NAME = "com.streamtablet"
DEFAULT_INTERVAL_MS = 2000  # milliseconds
USE_LIGHTWEIGHT = True  # Use /proc instead of top for lower overhead


def get_pid():
    """Get the PID of the app."""
    result = subprocess.run(
        ["adb", "shell", f"pidof {PACKAGE_NAME}"],
        capture_output=True, text=True
    )
    pid = result.stdout.strip()
    if not pid:
        return None
    return pid.split()[0]  # Take first PID if multiple


def get_thread_cpu(pid):
    """Get CPU usage for all threads of the process using top."""
    result = subprocess.run(
        ["adb", "shell", f"top -H -p {pid} -n 1 -b"],
        capture_output=True, text=True
    )

    threads = {}
    total_cpu = 0.0

    for line in result.stdout.split('\n'):
        # Parse thread lines: TID USER PR NI VIRT RES SHR S %CPU %MEM TIME+ THREAD
        # Example: 12111 u0_a375 10 -10 19G 258M 165M R 20.5 1.6 0:08.99 MediaCodec_loop
        match = re.match(
            r'\s*(\d+)\s+\S+\s+[\d-]+\s+[\d-]+\s+\S+\s+\S+\s+\S+\s+\S\s+([\d.]+)\s+[\d.]+\s+[\d:\.]+\s+(\S+)',
            line
        )
        if match:
            tid = match.group(1)
            cpu = float(match.group(2))
            thread_name = match.group(3)
            threads[thread_name] = cpu
            total_cpu += cpu

    return threads, total_cpu


# Store previous CPU times for delta calculation
_prev_thread_times = {}
_prev_total_time = 0
_prev_sample_time = 0


def get_thread_cpu_lightweight(pid):
    """Get CPU usage using /proc - much lower overhead than top."""
    global _prev_thread_times, _prev_total_time, _prev_sample_time

    # Get total system CPU time
    result = subprocess.run(
        ["adb", "shell", "cat /proc/stat | head -1"],
        capture_output=True, text=True
    )
    cpu_parts = result.stdout.strip().split()[1:]
    total_time = sum(int(x) for x in cpu_parts[:7])  # user, nice, system, idle, iowait, irq, softirq

    # Get thread CPU times
    result = subprocess.run(
        ["adb", "shell", f"cat /proc/{pid}/task/*/stat 2>/dev/null"],
        capture_output=True, text=True
    )

    current_times = {}
    thread_names = {}

    for line in result.stdout.strip().split('\n'):
        if not line:
            continue
        # Format: pid (name) state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime ...
        match = re.match(r'(\d+)\s+\(([^)]+)\)\s+\S+\s+(?:\S+\s+){10}(\d+)\s+(\d+)', line)
        if match:
            tid = match.group(1)
            name = match.group(2)
            utime = int(match.group(3))
            stime = int(match.group(4))
            current_times[tid] = utime + stime
            thread_names[tid] = name

    now = time.time()
    threads = {}
    total_cpu = 0.0

    if _prev_sample_time > 0:
        time_delta = total_time - _prev_total_time
        if time_delta > 0:
            for tid, cpu_time in current_times.items():
                prev_time = _prev_thread_times.get(tid, cpu_time)
                delta = cpu_time - prev_time
                # Convert to percentage (multiply by 100, divide by time delta, multiply by num CPUs)
                cpu_pct = (delta / time_delta) * 100 * 8  # Assuming 8 cores
                if tid in thread_names:
                    threads[thread_names[tid]] = cpu_pct
                    total_cpu += cpu_pct

    _prev_thread_times = current_times
    _prev_total_time = total_time
    _prev_sample_time = now

    return threads, total_cpu


def monitor_cpu(duration_seconds, interval_ms, output_file):
    """Monitor CPU usage and generate graph."""
    interval_seconds = interval_ms / 1000.0
    print(f"Monitoring {PACKAGE_NAME} for {duration_seconds} seconds...")
    print(f"Sample interval: {interval_ms}ms ({interval_seconds}s)")
    print(f"Mode: {'lightweight (/proc)' if USE_LIGHTWEIGHT else 'top -H'}")

    # Wait for app
    pid = get_pid()
    while not pid:
        print("Waiting for app to start...")
        time.sleep(1)
        pid = get_pid()

    print(f"Found PID: {pid}")

    # Data storage
    timestamps = []
    total_cpu_data = []
    thread_data = defaultdict(list)

    start_time = time.time()
    sample_count = 0

    try:
        while time.time() - start_time < duration_seconds:
            # Check if app is still running
            current_pid = get_pid()
            if current_pid != pid:
                if current_pid:
                    print(f"App restarted with new PID: {current_pid}")
                    pid = current_pid
                else:
                    print("App stopped. Waiting...")
                    time.sleep(1)
                    continue

            # Get CPU data
            if USE_LIGHTWEIGHT:
                threads, total_cpu = get_thread_cpu_lightweight(pid)
            else:
                threads, total_cpu = get_thread_cpu(pid)
            now = datetime.now()

            timestamps.append(now)
            total_cpu_data.append(total_cpu)

            # Track individual threads
            for thread_name, cpu in threads.items():
                thread_data[thread_name].append((now, cpu))

            sample_count += 1
            elapsed = time.time() - start_time
            print(f"[{elapsed:6.1f}s] Total CPU: {total_cpu:5.1f}%  Top: {max(threads.items(), key=lambda x: x[1]) if threads else ('N/A', 0)}")

            time.sleep(interval_seconds)

    except KeyboardInterrupt:
        print("\nStopped by user")

    if not timestamps:
        print("No data collected!")
        return

    print(f"\nCollected {sample_count} samples")

    # Generate graph
    generate_graph(timestamps, total_cpu_data, thread_data, output_file)


def generate_graph(timestamps, total_cpu_data, thread_data, output_file):
    """Generate the CPU usage graph."""
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 10), sharex=True)

    # Top graph: Total CPU usage
    ax1.plot(timestamps, total_cpu_data, 'b-', linewidth=2, label='Total CPU')
    ax1.fill_between(timestamps, total_cpu_data, alpha=0.3)
    ax1.set_ylabel('CPU Usage (%)')
    ax1.set_title(f'StreamTablet CPU Usage Over Time')
    ax1.legend(loc='upper right')
    ax1.grid(True, alpha=0.3)

    # Mark spikes (>50% CPU)
    spike_threshold = 50
    for i, (t, cpu) in enumerate(zip(timestamps, total_cpu_data)):
        if cpu > spike_threshold:
            ax1.axvline(x=t, color='red', alpha=0.3, linestyle='--')

    # Bottom graph: Top threads
    # Find threads with highest average CPU
    thread_avg = {}
    for thread_name, data in thread_data.items():
        if data:
            avg = sum(cpu for _, cpu in data) / len(data)
            thread_avg[thread_name] = avg

    # Get top 8 threads by average CPU
    top_threads = sorted(thread_avg.items(), key=lambda x: x[1], reverse=True)[:8]

    colors = plt.cm.tab10(range(len(top_threads)))
    for (thread_name, _), color in zip(top_threads, colors):
        data = thread_data[thread_name]
        times = [t for t, _ in data]
        cpus = [cpu for _, cpu in data]
        ax2.plot(times, cpus, '-', color=color, linewidth=1.5, label=thread_name, alpha=0.8)

    ax2.set_xlabel('Time')
    ax2.set_ylabel('CPU Usage (%)')
    ax2.set_title('Top Threads by CPU Usage')
    ax2.legend(loc='upper right', fontsize=8)
    ax2.grid(True, alpha=0.3)

    # Format x-axis
    ax2.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S'))
    plt.xticks(rotation=45)

    plt.tight_layout()
    plt.savefig(output_file, dpi=150)
    print(f"Graph saved to: {output_file}")

    # Also save raw data to CSV
    csv_file = output_file.rsplit('.', 1)[0] + '_data.csv'
    with open(csv_file, 'w') as f:
        f.write("timestamp,total_cpu," + ",".join(t[0] for t in top_threads) + "\n")
        for i, (t, total) in enumerate(zip(timestamps, total_cpu_data)):
            row = [t.strftime('%Y-%m-%d %H:%M:%S'), str(total)]
            for thread_name, _ in top_threads:
                data = thread_data[thread_name]
                cpu = data[i][1] if i < len(data) else 0
                row.append(str(cpu))
            f.write(",".join(row) + "\n")
    print(f"Data saved to: {csv_file}")

    # Show statistics
    print("\n=== CPU Statistics ===")
    print(f"Total CPU - Avg: {sum(total_cpu_data)/len(total_cpu_data):.1f}%, "
          f"Max: {max(total_cpu_data):.1f}%, Min: {min(total_cpu_data):.1f}%")
    print("\nTop threads by average CPU:")
    for thread_name, avg in top_threads:
        data = thread_data[thread_name]
        max_cpu = max(cpu for _, cpu in data) if data else 0
        print(f"  {thread_name:25s} - Avg: {avg:5.1f}%, Max: {max_cpu:5.1f}%")


if __name__ == "__main__":
    duration = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    interval = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_INTERVAL_MS
    output = sys.argv[3] if len(sys.argv) > 3 else "cpu_usage.png"

    monitor_cpu(duration, interval, output)
