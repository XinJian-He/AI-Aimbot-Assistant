#ifndef CARD_CAPTURE_H
#define CARD_CAPTURE_H

#include <Windows.h>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>

#include "FrameGrabber/FrameGrabber.h"

struct CardDeviceInfo {
    std::string name;
    std::string id;
};

struct CardModeInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    std::string format;
    std::string key;
    std::string label;
};

class CardCapture {
public:
    CardCapture();
    ~CardCapture();

    bool Init(int width, int height, const std::wstring& device = L"",
        const std::string& modeKey = "");
    void Reset();

    bool Capture(uint8_t* buffer, size_t bufferSize, UINT timeoutMs);

    LONGLONG GetLastPresentQpc() const { return m_lastQpcTicks; }

    int GetWidth() const { return m_outWidth; }
    int GetHeight() const { return m_outHeight; }
    int GetChannels() const { return 3; }

    std::string DescUtf8() const;
    const std::string& LastError() const { return m_error; }

    static bool HasCaptureCard();

    static std::vector<CardDeviceInfo> EnumDevices();

    static std::vector<CardModeInfo> EnumModes(const std::wstring& device);

    static std::string MakeModeKey(int width, int height, double fps,
        const std::string& format);

private:
    void SetupCoeffs(const fg::ModeDesc& mode);
    bool ConvertCenterToBGR(const fg::Frame& f, uint8_t* out) const;

    fg::Grabber m_grabber;
    std::wstring m_device;
    int m_outWidth = 0;
    int m_outHeight = 0;
    LONGLONG m_lastQpcTicks = 0;
    LONGLONG m_qpcFreq = 0;
    std::string m_error;
    std::chrono::steady_clock::duration m_frameInterval{};
    std::chrono::steady_clock::time_point m_nextFrame{};
    uint64_t m_lastFrameIndex = 0;

    int m_ys = 0, m_yo = 0;
    int m_rv = 0, m_gu = 0, m_gv = 0, m_bu = 0;
};

#endif
