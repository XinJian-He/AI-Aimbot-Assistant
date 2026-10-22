#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fg {

enum class PixelFormat { NV12, YUY2, P010, BGRA, RGB24, MJPEG, Unknown };

const wchar_t* PixelFormatName(PixelFormat f);

int64_t Now100ns();

struct DeviceDesc
{
    std::wstring name;
    std::wstring id;
};

struct ModeDesc
{
    uint32_t width = 0, height = 0;
    double fps = 0.0;
    PixelFormat format = PixelFormat::Unknown;

    bool fullRange = false;
    bool bt709 = true;

    bool operator==(const ModeDesc&) const = default;
};

struct Config
{

    std::wstring device;

    uint32_t width = 0, height = 0;
    double fps = 0.0;
    PixelFormat format = PixelFormat::Unknown;
};

struct Frame
{
    const uint8_t* data = nullptr;
    int32_t pitch = 0;
    uint32_t width = 0, height = 0;
    PixelFormat format = PixelFormat::Unknown;
    int64_t timestamp100ns = 0;
    uint64_t index = 0;

    Frame();
    ~Frame();
    Frame(Frame&&) noexcept;
    Frame& operator=(Frame&&) noexcept;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    bool Valid() const { return data != nullptr; }
    void Release();

private:
    friend class Grabber;
    struct Lock;
    std::unique_ptr<Lock> m_lock;
};

class Grabber
{
public:
    Grabber();
    ~Grabber();
    Grabber(const Grabber&) = delete;
    Grabber& operator=(const Grabber&) = delete;

    static std::vector<DeviceDesc> EnumDevices();

    static std::vector<ModeDesc> EnumModes(const std::wstring& device);

    bool Start(const Config& cfg, std::wstring* err = nullptr);
    void Stop();
    bool Running() const;

    bool WaitFrame(uint32_t timeoutMs) const;

    bool AcquireLatest(Frame& out);

    ModeDesc ActiveMode() const;
    DeviceDesc ActiveDevice() const;
    bool SourceIsMjpeg() const;
    uint64_t FramesCaptured() const;
    uint64_t FramesMerged() const;
    bool SignalLost() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}
