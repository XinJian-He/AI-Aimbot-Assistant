#include "makcu_provider.h"
// Explicit path avoids collision with this project's facade makcu.h.
#include "../../third_party/makcu-cpp/makcu-cpp/include/makcu.h"

namespace makcu_provider {
namespace {
class Compatible final : public Provider {
    makcu::Device device_;
    Status state_;
    bool failed_ = false;
    bool result(bool ok, const char* operation) {
        if (ok) return true;
        state_.lastError = device_.getLastError();
        if (state_.lastError.empty()) state_.lastError = operation;
        state_.connected = false;
        failed_ = true;
        // SDK may leave its connected flag true after a failed write. Never
        // append another command to a potentially incomplete serial command.
        device_.disconnect();
        return false;
    }
public:
    ~Compatible() override { close(); }
    bool connect(const std::string& port, int) override {
        state_ = {};
        state_.port = port;
        failed_ = false;
        if (!result(device_.connect(port, true), "Compatible SDK connect failed")) return false;
        // Upstream enables km.buttons(1) in initializeDevice. Disable it before
        // exposing the connection; all physical input belongs to Raw Input.
        if (!result(device_.enableButtonMonitoring(false), "Could not disable SDK button stream")) return false;
        state_.port = device_.getDeviceInfo().port;
        state_.baud = 4000000;
        state_.baudKnown = true;
        state_.connected = true;
        return true;
    }
    void close() override {
        if (device_.isConnected() && !failed_) releaseAll();
        device_.disconnect();
        state_.connected = false;
    }
    Status status() override {
        state_.connected = !failed_ && device_.isConnected();
        if (!state_.connected && state_.lastError.empty()) state_.lastError = device_.getLastError();
        return state_;
    }
    bool move(int dx, int dy) override {
        if (failed_ || !device_.isConnected()) return false;
        return result(device_.mouseMove(dx, dy), "Compatible SDK move write failed; reconnect required");
    }
    void button(unsigned char button, bool down) override {
        if (failed_ || !device_.isConnected()) return;
        int index = 0;
        while (index < 5 && button != (1u << index)) ++index;
        if (index == 5) return;
        const auto value = static_cast<makcu::MouseButton>(index);
        result(down ? device_.mouseDown(value) : device_.mouseUp(value), "Compatible SDK button write failed");
    }
    void wheel(int delta) override {
        if (!failed_ && device_.isConnected()) result(device_.mouseWheel(delta), "Compatible SDK wheel write failed");
    }
    void releaseAll() override { for (int i = 0; i < 5; ++i) button(static_cast<unsigned char>(1u << i), false); }
};
}
std::unique_ptr<Provider> makeCompatible() { return std::make_unique<Compatible>(); }
}
