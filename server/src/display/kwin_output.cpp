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

    char add_cmd[256];
    snprintf(add_cmd, sizeof(add_cmd),
             "kscreen-doctor output.%d.addCustomMode.%d.%d.%d.reduced",
             id, width, height, mhz);
    // Ignore error: mode may already exist.
    run_silent(add_cmd);

    char set_cmd[256];
    snprintf(set_cmd, sizeof(set_cmd),
             "kscreen-doctor output.%d.mode.%dx%d@%d",
             id, width, height, fps);
    int rc = run_silent(set_cmd);
    if (rc != 0) {
        LOG_ERROR("Failed to set mode %dx%d@%d on virtual output %d (rc=%d)",
                  width, height, fps, id, rc);
        return false;
    }

    LOG_INFO("Applied mode %dx%d@%d to virtual output %d", width, height, fps, id);
    // Give KWin a moment to settle before the capture pipeline initializes.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
