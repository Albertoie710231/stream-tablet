#include "audio/audio_router.hpp"
#include "util/logger.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sstream>

namespace stream_tablet {

namespace {

constexpr const char* kSinkName = "stream_tablet_sink";

std::string run_capture(const std::string& cmd) {
    std::string out;
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    // Trim trailing whitespace/newline.
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\r'))
        out.pop_back();
    return out;
}

int run_silent(const std::string& cmd) {
    return system((cmd + " >/dev/null 2>&1").c_str());
}

// Returns sink-input IDs currently routed to the given sink name.
// Uses `pactl list short sink-inputs` which outputs:
//   <id>\t<driver>\t<client>\t<sink-id>\t<sample-spec>\t<state>
// We cross-reference with `pactl list short sinks` to map name→id.
std::vector<std::string> list_sink_inputs() {
    std::vector<std::string> ids;
    std::string raw = run_capture("pactl list short sink-inputs 2>/dev/null");
    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        ids.push_back(line.substr(0, tab));
    }
    return ids;
}

std::string find_module_id_by_sink(const std::string& sink_name) {
    // `pactl list short modules` shows module id, name, argline.
    // Module line for a null sink includes "sink_name=<name>" in its args.
    std::string raw = run_capture("pactl list short modules 2>/dev/null");
    std::istringstream ss(raw);
    std::string line;
    std::string needle = "sink_name=" + sink_name;
    while (std::getline(ss, line)) {
        if (line.find("module-null-sink") == std::string::npos) continue;
        if (line.find(needle) == std::string::npos) continue;
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        return line.substr(0, tab);
    }
    return "";
}

bool sink_exists(const std::string& sink_name) {
    std::string raw = run_capture("pactl list short sinks 2>/dev/null");
    return raw.find(sink_name) != std::string::npos;
}

}  // namespace

AudioRouter::~AudioRouter() {
    end();
}

void AudioRouter::cleanup_stale() {
    std::string id = find_module_id_by_sink(kSinkName);
    if (!id.empty()) {
        LOG_WARN("Found stale %s from previous run (module %s), unloading",
                 kSinkName, id.c_str());
        run_silent("pactl unload-module " + id);
    }
}

bool AudioRouter::begin() {
    if (m_active) return true;

    // Record the current default sink so we can restore it on end().
    m_original_default_sink = run_capture("pactl get-default-sink 2>/dev/null");
    if (m_original_default_sink.empty()) {
        LOG_ERROR("pactl get-default-sink failed — is PipeWire/PulseAudio running?");
        return false;
    }

    // If a leftover sink from a previous crash is still around, nuke it so
    // we can create a fresh one with a known module id.
    if (sink_exists(kSinkName)) {
        std::string stale = find_module_id_by_sink(kSinkName);
        if (!stale.empty()) run_silent("pactl unload-module " + stale);
    }

    // Load the null sink. `pactl load-module` prints the new module id.
    std::string cmd =
        "pactl load-module module-null-sink "
        "sink_name=" + std::string(kSinkName) + " "
        "sink_properties=device.description=StreamTablet "
        "2>/dev/null";
    m_module_id = run_capture(cmd);
    if (m_module_id.empty()) {
        LOG_ERROR("Failed to load module-null-sink");
        return false;
    }

    // Make the null sink the system default. New apps will auto-route here.
    if (run_silent("pactl set-default-sink " + std::string(kSinkName)) != 0) {
        LOG_ERROR("Failed to set %s as default sink", kSinkName);
        run_silent("pactl unload-module " + m_module_id);
        m_module_id.clear();
        return false;
    }

    // Move existing streams (music players, browser tabs, etc.) onto it.
    for (const auto& id : list_sink_inputs()) {
        run_silent("pactl move-sink-input " + id + " " + kSinkName);
    }

    m_active = true;
    LOG_INFO("Audio routed exclusively to tablet (original default: %s)",
             m_original_default_sink.c_str());
    return true;
}

void AudioRouter::end() {
    if (!m_active) return;

    // Restore original default sink first, so streams moved back have a
    // home to go to if they were following the default.
    if (!m_original_default_sink.empty()) {
        run_silent("pactl set-default-sink " + m_original_default_sink);
    }

    // Move any streams that are still on the null sink back to the original.
    // We do this by listing sink-inputs and moving everything — pactl is a
    // no-op if the stream is already on the target sink.
    if (!m_original_default_sink.empty()) {
        for (const auto& id : list_sink_inputs()) {
            run_silent("pactl move-sink-input " + id + " " + m_original_default_sink);
        }
    }

    if (!m_module_id.empty()) {
        run_silent("pactl unload-module " + m_module_id);
        m_module_id.clear();
    }

    LOG_INFO("Audio routing restored to %s", m_original_default_sink.c_str());
    m_original_default_sink.clear();
    m_active = false;
}

}  // namespace stream_tablet
