#include "display/kwin_output.hpp"
#include "util/logger.hpp"

#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string>
#include <thread>
#include <chrono>

namespace stream_tablet {

namespace {

std::string run_capture(const std::string& cmd) {
    std::string out;
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return out;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    return out;
}

int run_silent(const std::string& cmd) {
    return system((cmd + " >/dev/null 2>&1").c_str());
}

// Strip ANSI color escape sequences (\x1b[...m) from kscreen-doctor output.
std::string strip_ansi(const std::string& in) {
    static const std::regex re("\x1b\\[[0-9;]*m");
    return std::regex_replace(in, re, "");
}

std::string config_path() {
    const char* home = getenv("HOME");
    std::string base = home ? home : "/tmp";
    return base + "/.cache/stream-tablet-display";
}

// Returns the refresh rate of the output's currently active mode (marked '*'),
// or 0 if it cannot be determined.
double active_refresh(int id) {
    std::string clean = strip_ansi(run_capture("kscreen-doctor -o 2>/dev/null"));
    // Find the block for this output, then the mode token ending in '*'.
    std::string needle = "Output: " + std::to_string(id) + " ";
    auto pos = clean.find(needle);
    if (pos == std::string::npos) return 0.0;
    auto end = clean.find("\nOutput: ", pos + 1);
    std::string block = clean.substr(pos, end == std::string::npos ? std::string::npos : end - pos);

    std::regex active_re(R"((\d+)x(\d+)@([0-9.]+)\*)");
    std::smatch m;
    if (std::regex_search(block, m, active_re)) {
        return std::stod(m[3]);
    }
    return 0.0;
}

// True if the output already advertises a mode at this size and (rounded)
// refresh, so we do not pile up a fresh custom mode on every connect.
bool has_mode(int id, int width, int height, int fps) {
    std::string clean = strip_ansi(run_capture("kscreen-doctor -o 2>/dev/null"));
    std::string needle = "Output: " + std::to_string(id) + " ";
    auto pos = clean.find(needle);
    if (pos == std::string::npos) return false;
    auto end = clean.find("\nOutput: ", pos + 1);
    std::string block = clean.substr(pos, end == std::string::npos ? std::string::npos : end - pos);

    std::string size = std::to_string(width) + "x" + std::to_string(height) + "@";
    size_t p = 0;
    while ((p = block.find(size, p)) != std::string::npos) {
        p += size.size();
        double r = atof(block.c_str() + p);
        if (r >= fps - 1.0 && r < fps + 1.0) return true;
    }
    return false;
}

// Finds the mode id at the requested size whose refresh is closest to `fps`.
// Returns false if the output advertises nothing at that size.
//
// This matters more than it looks. kscreen-doctor's "WxH@120" spec matches on
// the rounded refresh, so it will happily select a 119.78Hz mode when a 119.99Hz
// one exists. The tablet panel runs at 120.00001Hz, and a source/sink mismatch
// of 0.22Hz means the panel needs one extra frame roughly every 4.5 seconds —
// a repeated frame, felt as a periodic hitch no amount of pipeline smoothing can
// remove. At 0.01Hz that interval stretches to about 100 seconds.
bool find_closest_mode(int output_id, int width, int height, int fps,
                       int& mode_id_out, double& refresh_out) {
    std::string clean = strip_ansi(run_capture("kscreen-doctor -o 2>/dev/null"));
    std::string needle = "Output: " + std::to_string(output_id) + " ";
    auto pos = clean.find(needle);
    if (pos == std::string::npos) return false;
    auto end = clean.find("\nOutput: ", pos + 1);
    std::string block = clean.substr(pos, end == std::string::npos ? std::string::npos : end - pos);

    // Mode entries look like "19:2960x1848@119.78" with '*' marking the active one.
    std::regex mode_re(R"((\d+):(\d+)x(\d+)@([0-9.]+))");
    auto begin = std::sregex_iterator(block.begin(), block.end(), mode_re);
    auto stop = std::sregex_iterator();

    bool found = false;
    double best_delta = 0.0;
    for (auto it = begin; it != stop; ++it) {
        int w = std::stoi((*it)[2]);
        int h = std::stoi((*it)[3]);
        if (w != width || h != height) continue;
        double hz = std::stod((*it)[4]);
        double delta = hz > fps ? hz - fps : fps - hz;
        if (!found || delta < best_delta) {
            found = true;
            best_delta = delta;
            mode_id_out = std::stoi((*it)[1]);
            refresh_out = hz;
        }
    }
    return found;
}

bool find_virtual_output_id(int& id_out) {
    std::string raw = run_capture("kscreen-doctor -o 2>/dev/null");
    if (raw.empty()) {
        LOG_WARN("kscreen-doctor returned nothing (is it installed?)");
        return false;
    }
    std::string clean = strip_ansi(raw);

    // Each output starts with "Output: <id> <name> <uuid>"
    std::regex line_re(R"(Output:\s*(\d+)\s+(\S+))");
    auto begin = std::sregex_iterator(clean.begin(), clean.end(), line_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string name = (*it)[2];
        if (name.find("Virtual") != std::string::npos) {
            id_out = std::stoi((*it)[1]);
            LOG_INFO("Found KWin virtual output: id=%d name=%s", id_out, name.c_str());
            return true;
        }
    }
    LOG_WARN("No virtual output found in kscreen-doctor output");
    return false;
}

}  // namespace

bool apply_virtual_output_mode(int width, int height, int fps) {
    if (width <= 0 || height <= 0 || fps <= 0) return false;

    int id = -1;
    if (!find_virtual_output_id(id)) return false;

    // kscreen-doctor expects millihertz.
    int mhz = fps * 1000;

    // Only create a custom mode if nothing suitable exists. addCustomMode
    // computes slightly different timings each call, so calling it every
    // connect accumulates a new near-duplicate mode forever.
    if (!has_mode(id, width, height, fps)) {
        char add_cmd[256];
        snprintf(add_cmd, sizeof(add_cmd),
                 "kscreen-doctor output.%d.addCustomMode.%d.%d.%d.reduced",
                 id, width, height, mhz);
        run_silent(add_cmd);
    }

    // Already at the requested refresh? Then don't touch it — re-setting the
    // mode makes KWin tear the screencast stream down and renegotiate, which
    // is how the stream ends up pinned to the pre-switch refresh rate.
    // Only leave it alone if it is already on the *best* available refresh —
    // "close enough to the integer" is what left us 0.22 Hz off the panel.
    double before = active_refresh(id);
    int best_id = -1;
    double best_hz = 0.0;
    bool have_best = find_closest_mode(id, width, height, fps, best_id, best_hz);
    if (have_best && before > 0.0 &&
        (before > best_hz ? before - best_hz : best_hz - before) < 0.005) {
        LOG_INFO("Virtual output %d already at %dx%d@%.2f (best available), leaving it alone",
                 id, width, height, before);
        return true;
    }

    // Pick the closest available refresh explicitly rather than letting
    // kscreen match on the rounded rate.
    char set_cmd[256];
    int mode_id = -1;
    double mode_hz = 0.0;
    if (find_closest_mode(id, width, height, fps, mode_id, mode_hz)) {
        LOG_INFO("Selecting mode %d: %dx%d@%.2f (requested %d, delta %.2f Hz)",
                 mode_id, width, height, mode_hz, fps,
                 mode_hz > fps ? mode_hz - fps : fps - mode_hz);
        snprintf(set_cmd, sizeof(set_cmd), "kscreen-doctor output.%d.mode.%d", id, mode_id);
    } else {
        snprintf(set_cmd, sizeof(set_cmd),
                 "kscreen-doctor output.%d.mode.%dx%d@%d",
                 id, width, height, fps);
    }
    int rc = run_silent(set_cmd);
    if (rc != 0) {
        LOG_ERROR("Failed to set mode %dx%d@%d on virtual output %d (rc=%d)",
                  width, height, fps, id, rc);
        return false;
    }

    // Wait until KWin reports the new refresh rather than guessing at 200ms.
    // If we reconnect the PipeWire stream too early it negotiates against the
    // OLD mode and caps max_framerate there for the whole session.
    double now_hz = 0.0;
    for (int i = 0; i < 40; i++) {  // up to ~2s
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        now_hz = active_refresh(id);
        if (now_hz >= fps - 1.0 && now_hz < fps + 1.0) break;
    }

    if (now_hz < fps - 1.0 || now_hz >= fps + 1.0) {
        LOG_WARN("Virtual output %d did not reach %d Hz (now %.2f Hz) — "
                 "capture will be capped at the lower rate", id, fps, now_hz);
    } else {
        LOG_INFO("Applied mode %dx%d@%.2f to virtual output %d", width, height, now_hz, id);
    }
    return true;
}

bool load_saved_display_mode(int& width, int& height, int& fps) {
    FILE* f = fopen(config_path().c_str(), "r");
    if (!f) return false;
    int w = 0, h = 0, r = 0;
    int n = fscanf(f, "%d %d %d", &w, &h, &r);
    fclose(f);
    if (n != 3 || w <= 0 || h <= 0 || r <= 0) return false;
    width = w;
    height = h;
    fps = r;
    LOG_INFO("Loaded saved display mode: %dx%d@%d", width, height, fps);
    return true;
}

void save_display_mode(int width, int height, int fps) {
    FILE* f = fopen(config_path().c_str(), "w");
    if (!f) {
        LOG_WARN("Failed to save display mode to %s", config_path().c_str());
        return;
    }
    fprintf(f, "%d %d %d\n", width, height, fps);
    fclose(f);
    LOG_INFO("Saved display mode %dx%d@%d for next launch", width, height, fps);
}

}  // namespace stream_tablet
