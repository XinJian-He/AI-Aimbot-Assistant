#pragma once

#include "FrameGrabber.h"

#include <Windows.h>

#include <memory>
#include <vector>

namespace fg {

class DShowCapture
{
public:
    DShowCapture();
    ~DShowCapture();
    DShowCapture(const DShowCapture&) = delete;
    DShowCapture& operator=(const DShowCapture&) = delete;

    static std::vector<DeviceDesc> EnumDevices();
    static std::vector<ModeDesc> EnumModes(const std::wstring& device);

    bool Start(const Config& cfg, HANDLE frameEvent, std::wstring* err);
    void Stop();
    bool Running() const;
    bool SourceIsMjpeg() const;

    struct Sample
    {
        std::shared_ptr<std::vector<uint8_t>> hold;
        const uint8_t* data = nullptr;
        int32_t pitch = 0;
        int64_t timestamp100ns = 0;
        uint64_t index = 0;
    };
    bool AcquireLatest(Sample& out);

    ModeDesc ActiveMode() const;
    DeviceDesc ActiveDevice() const;
    uint64_t FramesCaptured() const;
    uint64_t FramesDropped() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}
