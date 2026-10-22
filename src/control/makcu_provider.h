#pragma once
#include <memory>
#include <string>

// Production supports only Compatible SDK / negotiated 4M.

namespace makcu_provider {
struct Config {
    int id;
    const char* name;
    const char* sdkVersion;
    const char* protocol;
    const char* baudPolicy;
};
struct Status {
    bool connected = false;
    std::string port;
    int baud = 0;
    bool baudKnown = false;
    std::string lastError;
};
// Facade serializes all calls, including status and destruction. Adapters own
// transport threads; none may call back into the facade while joining them.
class Provider {
public:
    virtual ~Provider() = default;
    virtual bool connect(const std::string& port, int requestedBaud) = 0; // empty = AUTO only
    virtual void close() = 0;
    virtual Status status() = 0;
    virtual bool move(int dx, int dy) = 0;
    virtual void button(unsigned char button, bool down) = 0;
    virtual void wheel(int delta) = 0;
    virtual void releaseAll() = 0;
};
std::unique_ptr<Provider> makeCompatible();
}
