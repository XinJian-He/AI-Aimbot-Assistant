#include "makcu.h"
#include "makcu_provider.h"
#include "raw_input.h"
#include "makcu2.h"
#include <algorithm>
#include <cstdio>
#include <exception>
#include <mutex>

extern void PushLog(const std::string&);
namespace {
using namespace makcu_provider;
std::mutex mutex;
constexpr int selected = 2;
std::unique_ptr<Provider> provider;
Status lastStatus;
std::string facadeError;

const Config* config(int id) {
    return id == 2 ? &scheme2::config : nullptr;
}
std::unique_ptr<Provider> create(int id) {
    return id == 2 ? scheme2::create() : nullptr;
}
void error(const std::string& text) {
    facadeError = text;
    PushLog("[MAKCU] " + text);
}
Status status() {
    if (provider) lastStatus = provider->status();
    if (!facadeError.empty()) {
        Status copy = lastStatus;
        copy.lastError = facadeError;
        return copy;
    }
    return lastStatus;
}
void closeLocked() {
    if (provider) {
        provider->close();
        lastStatus = provider->status();
        provider.reset();
    }
    lastStatus.connected = false;
}
// JSON escaping is kept local: these diagnostics must not depend on the backend.
std::string quote(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20) {
                char escape[7];
                std::snprintf(escape, sizeof(escape), "\\u%04x", c);
                result += escape;
            } else result += static_cast<char>(c);
        }
    }
    return result + '"';
}
template<class Action> bool invoke(Action action) {
    if (!provider) return false;
    try { return action(*provider); }
    catch (const std::exception& e) { error(e.what()); }
    catch (...) { error("Provider operation failed with an unknown exception"); }
    closeLocked();
    return false;
}
}

bool makcu_select_scheme(int scheme) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!config(scheme)) { error("Unknown or disabled MAKCU scheme"); return false; }
    return true;
}

int makcu_get_scheme() { std::lock_guard<std::mutex> lock(mutex); return selected; }

bool makcu_init(const char* portName, int baudRate) {
    std::lock_guard<std::mutex> lock(mutex);
    closeLocked();
    facadeError.clear();
    lastStatus = {};
    if (!portName || !*portName) { error("Use AUTO or an explicit COM port"); return false; }
    const std::string port = _stricmp(portName, "AUTO") == 0 ? "" : portName;
    lastStatus.port = port;
    if (!config(selected)) { error("Selected MAKCU scheme is disabled"); return false; }
    try {
        provider = create(selected);
        const bool ok = provider->connect(port, baudRate);
        lastStatus = provider->status();
        if (ok && lastStatus.connected) return true;
        if (lastStatus.lastError.empty()) lastStatus.lastError = "MAKCU connection failed";
        // Failed connect releases ownership; I/O failures retain it until close.
        const auto failed = lastStatus;
        closeLocked();
        lastStatus = failed;
        lastStatus.connected = false;
    } catch (const std::exception& e) { error(e.what()); closeLocked(); }
    catch (...) { error("MAKCU connection failed with an unknown exception"); closeLocked(); }
    return false;
}
void makcu_close() { std::lock_guard<std::mutex> lock(mutex); closeLocked(); }
bool makcu_is_connected() { std::lock_guard<std::mutex> lock(mutex); return status().connected; }
bool makcu_autodetect(char* outPort, int* outBaud) {
    // Preserve the historical 32-byte output buffer contract.
    if (outPort) outPort[0] = '\0';
    if (outBaud) *outBaud = 0;
    if (!makcu_init("AUTO", 115200)) return false;
    std::lock_guard<std::mutex> lock(mutex);
    const auto current = status();
    if (!current.connected) return false;
    if (outPort) std::snprintf(outPort, 32, "%s", current.port.c_str());
    if (outBaud) *outBaud = current.baudKnown ? current.baud : 0;
    return true;
}
std::string makcu_get_info_json() {
    std::lock_guard<std::mutex> lock(mutex);
    const auto current = status();
    const auto* cfg = config(selected);
    return "{\"scheme\":" + std::to_string(selected) +
        ",\"connected\":" + (current.connected ? "true" : "false") +
        ",\"port\":" + quote(current.port) + ",\"baud\":" + std::to_string(current.baud) +
        ",\"baudKnown\":" + (current.baudKnown ? "true" : "false") +
        ",\"protocol\":" + quote(cfg ? cfg->protocol : "disabled") +
        ",\"sdkVersion\":" + quote(cfg ? cfg->sdkVersion : "") +
        ",\"lastError\":" + quote(current.lastError) + "}";
}
std::string makcu_get_schemes_json() {
    const auto& cfg = scheme2::config;
    return "[{\"id\":2,\"name\":" + quote(cfg.name) +
        ",\"sdkVersion\":" + quote(cfg.sdkVersion) + ",\"protocol\":" + quote(cfg.protocol) +
        ",\"baudPolicy\":" + quote(cfg.baudPolicy) + "}]";
}

const char* makcu_get_last_error() {
    thread_local std::string result;
    std::lock_guard<std::mutex> lock(mutex);
    result = status().lastError;
    return result.c_str();
}

// Local Raw Input only; never query SDK streams or fall back to system polling.
int makcu_button_pressed(unsigned char btn) {
    int vk = 0;
    switch (btn) {
    case MAKCU_BTN_LEFT: vk = VK_LBUTTON; break;
    case MAKCU_BTN_RIGHT: vk = VK_RBUTTON; break;
    case MAKCU_BTN_MIDDLE: vk = VK_MBUTTON; break;
    case MAKCU_BTN_SIDE1: vk = VK_XBUTTON1; break;
    case MAKCU_BTN_SIDE2: vk = VK_XBUTTON2; break;
    default: return 0;
    }
    return RawInput::IsPressed(vk) ? 1 : 0;
}
bool makcu_is_left_down() { return makcu_button_pressed(MAKCU_BTN_LEFT) != 0; }
bool makcu_is_right_down() { return makcu_button_pressed(MAKCU_BTN_RIGHT) != 0; }
bool makcu_is_middle_down() { return makcu_button_pressed(MAKCU_BTN_MIDDLE) != 0; }
bool makcu_is_side1_down() { return makcu_button_pressed(MAKCU_BTN_SIDE1) != 0; }
bool makcu_is_side2_down() { return makcu_button_pressed(MAKCU_BTN_SIDE2) != 0; }

bool makcu_mouse_move(int dx, int dy) {
    std::lock_guard<std::mutex> lock(mutex);
    return invoke([&](Provider& p) { return p.move((std::clamp)(dx, -32768, 32767), (std::clamp)(dy, -32768, 32767)); });
}
void makcu_mouse_button(unsigned char b, int state) {
    std::lock_guard<std::mutex> lock(mutex);
    invoke([&](Provider& p) { p.button(b, state != 0); return true; });
}
void makcu_mouse_left(int s) { makcu_mouse_button(MAKCU_BTN_LEFT, s); }
void makcu_mouse_right(int s) { makcu_mouse_button(MAKCU_BTN_RIGHT, s); }
void makcu_mouse_middle(int s) { makcu_mouse_button(MAKCU_BTN_MIDDLE, s); }
void makcu_mouse_wheel(char wheel) {
    std::lock_guard<std::mutex> lock(mutex);
    invoke([&](Provider& p) { p.wheel((std::clamp)(static_cast<int>(wheel), -127, 127)); return true; });
}
void makcu_mouse_all(unsigned char buttons, short dx, short dy, char wheel) {
    std::lock_guard<std::mutex> lock(mutex);
    invoke([&](Provider& p) {
        if (!p.move(dx, dy)) return false;
        p.wheel((std::clamp)(static_cast<int>(wheel), -127, 127));
        for (int i = 0; i < 5; ++i) p.button(static_cast<unsigned char>(1u << i), (buttons & (1u << i)) != 0);
        return true;
    });
}
void makcu_release_all() {
    std::lock_guard<std::mutex> lock(mutex);
    invoke([](Provider& p) { p.releaseAll(); return true; });
}
static void warnKeyboard() {
    std::lock_guard<std::mutex> lock(mutex);
    error("Keyboard injection is not implemented by the MAKCU adapters");
}
void makcu_key_down(unsigned char) { warnKeyboard(); }
void makcu_key_up(unsigned char) { warnKeyboard(); }
void makcu_modifier_down(unsigned char) { warnKeyboard(); }
void makcu_modifier_up(unsigned char) { warnKeyboard(); }
