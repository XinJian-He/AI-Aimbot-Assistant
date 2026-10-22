#include "CardCapture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <thread>

extern void PushLog(const std::string& s);

namespace {

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
        static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
        out.data(), len, nullptr, nullptr);
    return out;
}

inline uint8_t Clamp8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

inline LONGLONG Ns100ToQpcTicks(int64_t ts100, LONGLONG freq) {
    return (ts100 / 10000000LL) * freq +
        (ts100 % 10000000LL) * freq / 10000000LL;
}

std::string FormatToToken(fg::PixelFormat f) {
    switch (f) {
    case fg::PixelFormat::NV12:  return "NV12";
    case fg::PixelFormat::YUY2:  return "YUY2";
    case fg::PixelFormat::P010:  return "P010";
    case fg::PixelFormat::BGRA:  return "RGB32";
    case fg::PixelFormat::RGB24: return "RGB24";
    case fg::PixelFormat::MJPEG: return "MJPG";
    default:                     return "";
    }
}

fg::PixelFormat FormatFromToken(const std::string& s) {
    if (s == "NV12") return fg::PixelFormat::NV12;
    if (s == "YUY2") return fg::PixelFormat::YUY2;
    if (s == "P010") return fg::PixelFormat::P010;
    if (s == "RGB32" || s == "BGRA") return fg::PixelFormat::BGRA;
    if (s == "RGB24") return fg::PixelFormat::RGB24;
    if (s == "MJPG" || s == "MJPEG") return fg::PixelFormat::MJPEG;
    return fg::PixelFormat::Unknown;
}

bool ParseModeKey(const std::string& key, int& w, int& h, double& fps,
    std::string& fmt) {
    if (key.empty()) {
        return false;
    }
    char fmtbuf[16] = {0};
    const int n = sscanf_s(key.c_str(), "%dx%d@%lf/%15s",
        &w, &h, &fps, fmtbuf, static_cast<unsigned>(sizeof(fmtbuf)));
    if (n != 4 || w <= 0 || h <= 0 || !std::isfinite(fps) || fps <= 0.0) {
        return false;
    }
    fmt = (n >= 4) ? fmtbuf : "";
    return true;
}

}

std::string CardCapture::MakeModeKey(int width, int height, double fps,
    const std::string& format) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%dx%d@%.3f/%s", width, height, fps,
        format.c_str());
    return buf;
}

CardCapture::CardCapture() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    m_qpcFreq = f.QuadPart;
}

CardCapture::~CardCapture() {
    m_grabber.Stop();
}

std::string CardCapture::DescUtf8() const {
    if (!m_grabber.Running()) {
        return {};
    }
    const fg::ModeDesc mode = m_grabber.ActiveMode();
    char buf[128];
    std::snprintf(buf, sizeof(buf), "  %ux%u @ %.1f fps %s",
        mode.width, mode.height, mode.fps,
        WideToUtf8(fg::PixelFormatName(mode.format)).c_str());
    return WideToUtf8(m_grabber.ActiveDevice().name) + buf;
}

std::vector<CardDeviceInfo> CardCapture::EnumDevices() {
    std::vector<CardDeviceInfo> out;
    for (const auto& dev : fg::Grabber::EnumDevices()) {
        out.push_back({WideToUtf8(dev.name), WideToUtf8(dev.id)});
    }
    return out;
}

std::vector<CardModeInfo> CardCapture::EnumModes(const std::wstring& device) {
    std::vector<CardModeInfo> out;
    for (const auto& m : fg::Grabber::EnumModes(device)) {
        const std::string fmt = FormatToToken(m.format);
        if (fmt.empty() || m.width == 0 || m.height == 0) {
            continue;
        }
        CardModeInfo info;
        info.width = static_cast<int>(m.width);
        info.height = static_cast<int>(m.height);
        info.fps = m.fps;
        info.format = fmt;
        info.key = MakeModeKey(info.width, info.height, info.fps, fmt);

        char label[80];
        std::snprintf(label, sizeof(label), "%d\xC3\x97%d \xC2\xB7 %gfps \xC2\xB7 %s",
            info.width, info.height, info.fps, fmt.c_str());
        info.label = label;

        bool dup = false;
        for (const auto& e : out) {
            if (e.key == info.key) { dup = true; break; }
        }
        if (!dup) {
            out.push_back(std::move(info));
        }
    }

    std::sort(out.begin(), out.end(), [](const CardModeInfo& a, const CardModeInfo& b) {
        const long long pa = 1LL * a.width * a.height;
        const long long pb = 1LL * b.width * b.height;
        if (pa != pb) return pa > pb;
        return a.fps > b.fps;
    });
    return out;
}

bool CardCapture::HasCaptureCard() {
    for (const auto& dev : fg::Grabber::EnumDevices()) {
        std::wstring name = dev.name;
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        if (name.find(L"gc553") != std::wstring::npos ||
            name.find(L"live gamer") != std::wstring::npos ||
            name.find(L"avermedia") != std::wstring::npos ||
            name.find(L"capture") != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool CardCapture::Init(int width, int height, const std::wstring& device,
    const std::string& modeKey) {
    Reset();
    m_error.clear();
    m_outWidth = width;
    m_outHeight = height;
    m_device = device;

    std::wstring err;

    int mw = 0, mh = 0;
    double mfps = 0.0;
    std::string mfmt;
    const bool haveMode = ParseModeKey(modeKey, mw, mh, mfps, mfmt);
    if (!modeKey.empty() && (!haveMode || FormatFromToken(mfmt) == fg::PixelFormat::Unknown)) {
        m_error = "采集格式无效，请重新选择设备提供的格式";
        return false;
    }

    auto tryDevice = [&](const std::wstring& dev) -> bool {
        if (haveMode) {
            fg::Config want;
            want.device = dev;
            want.width = static_cast<uint32_t>(mw);
            want.height = static_cast<uint32_t>(mh);
            want.fps = mfps;
            want.format = FormatFromToken(mfmt);
            // Explicit selection is a contract, not a hint. Never silently
            // reopen another mode/FPS when this one fails.
            return m_grabber.Start(want, &err);
        }
        fg::Config pref;
        pref.device = dev;
        pref.width = 1920;
        pref.height = 1080;
        if (m_grabber.Start(pref, &err)) {
            return true;
        }
        fg::Config any;
        any.device = dev;
        return m_grabber.Start(any, &err);
    };

    bool started = tryDevice(device);
    if (!started) {
        m_error = "采集卡打开失败: " + WideToUtf8(err);
        return false;
    }

    const fg::ModeDesc mode = m_grabber.ActiveMode();
    if (mode.width < static_cast<uint32_t>(width) ||
        mode.height < static_cast<uint32_t>(height)) {
        m_grabber.Stop();
        m_error = "采集格式分辨率小于当前采集范围";
        return false;
    }
    const double fps = haveMode ? mfps : mode.fps;
    if (!std::isfinite(fps) || fps <= 0.0) {
        m_grabber.Stop();
        m_error = "设备未报告有效帧率，请选择明确的采集格式";
        return false;
    }
    m_frameInterval = std::chrono::ceil<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / fps));
    m_nextFrame = std::chrono::steady_clock::now();

    SetupCoeffs(mode);
    char rate[96];
    std::snprintf(rate, sizeof(rate), "；输出上限 %.3f FPS", fps);
    PushLog("[采集卡] 已生效: " + DescUtf8() + rate);
    return true;
}

void CardCapture::Reset() {
    m_grabber.Stop();
    m_lastQpcTicks = 0;
    m_frameInterval = {};
    m_nextFrame = {};
    m_lastFrameIndex = 0;
}

void CardCapture::SetupCoeffs(const fg::ModeDesc& mode) {
    const double cs = mode.fullRange ? 1.0 : 255.0 / 224.0;
    const double rv = mode.bt709 ? 1.5748 : 1.402;
    const double gu = mode.bt709 ? 0.18732 : 0.344136;
    const double gv = mode.bt709 ? 0.46812 : 0.714136;
    const double bu = mode.bt709 ? 1.8556 : 1.772;

    m_ys = mode.fullRange ? 65536 : static_cast<int>(std::lround(65536.0 * 255.0 / 219.0));
    m_yo = mode.fullRange ? 0 : 16;
    m_rv = static_cast<int>(std::lround(65536.0 * rv * cs));
    m_gu = static_cast<int>(std::lround(65536.0 * gu * cs));
    m_gv = static_cast<int>(std::lround(65536.0 * gv * cs));
    m_bu = static_cast<int>(std::lround(65536.0 * bu * cs));
}

bool CardCapture::Capture(uint8_t* buffer, size_t bufferSize, UINT timeoutMs) {
    const size_t need =
        static_cast<size_t>(m_outWidth) * m_outHeight * 3;
    if (!buffer || bufferSize < need || m_outWidth <= 0 || m_outHeight <= 0) {
        return false;
    }
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    // Wait before acquiring, so a fast driver overwrites its one-frame slot
    // and we consume the freshest sample at the selected rate (no frame queue).
    if (Clock::now() < m_nextFrame) {
        std::this_thread::sleep_until(std::min(m_nextFrame, deadline));
        if (Clock::now() < m_nextFrame) return false;
    }
    fg::Frame f;
    for (;;) {
        const auto remaining = deadline - Clock::now();
        const auto waitMs = remaining > Clock::duration::zero()
            ? std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() : 0;
        if (!m_grabber.WaitFrame(static_cast<UINT>(waitMs))) return false;
        if (m_grabber.AcquireLatest(f) && f.index > m_lastFrameIndex) break;
        if (Clock::now() >= deadline) return false;
    }
    m_lastFrameIndex = f.index;
    if (f.width < static_cast<uint32_t>(m_outWidth) ||
        f.height < static_cast<uint32_t>(m_outHeight)) {
        return false;
    }
    if (!ConvertCenterToBGR(f, buffer)) {
        return false;
    }
    m_lastQpcTicks = Ns100ToQpcTicks(f.timestamp100ns, m_qpcFreq);
    // No catch-up bursts after stalls or slow conversion.
    m_nextFrame = Clock::now() + m_frameInterval;
    return true;
}

bool CardCapture::ConvertCenterToBGR(const fg::Frame& f, uint8_t* out) const {
    const int W = m_outWidth, H = m_outHeight;

    const int x0 = static_cast<int>((f.width - W) / 2) & ~1;
    const int y0 = static_cast<int>((f.height - H) / 2) & ~1;
    const ptrdiff_t pitch = f.pitch;

    switch (f.format) {
    case fg::PixelFormat::NV12: {
        const uint8_t* yBase = f.data;
        const uint8_t* uvBase = f.data + pitch * f.height;
        for (int y = 0; y < H; ++y) {
            const uint8_t* yRow = yBase + (y0 + y) * pitch;
            const uint8_t* uvRow = uvBase + ((y0 + y) >> 1) * pitch;
            uint8_t* dst = out + static_cast<size_t>(y) * W * 3;
            for (int x = 0; x < W; ++x) {
                const int px = x0 + x;
                const int Yq = (yRow[px] - m_yo) * m_ys;
                const uint8_t* uv = uvRow + (px & ~1);
                const int U = uv[0] - 128;
                const int V = uv[1] - 128;
                dst[0] = Clamp8((Yq + m_bu * U) >> 16);
                dst[1] = Clamp8((Yq - m_gu * U - m_gv * V) >> 16);
                dst[2] = Clamp8((Yq + m_rv * V) >> 16);
                dst += 3;
            }
        }
        return true;
    }
    case fg::PixelFormat::YUY2: {
        for (int y = 0; y < H; ++y) {
            const uint8_t* row = f.data + (y0 + y) * pitch;
            uint8_t* dst = out + static_cast<size_t>(y) * W * 3;
            for (int x = 0; x < W; ++x) {
                const int px = x0 + x;
                const uint8_t* pair = row + (px & ~1) * 2;
                const int Yq = (row[px * 2] - m_yo) * m_ys;
                const int U = pair[1] - 128;
                const int V = pair[3] - 128;
                dst[0] = Clamp8((Yq + m_bu * U) >> 16);
                dst[1] = Clamp8((Yq - m_gu * U - m_gv * V) >> 16);
                dst[2] = Clamp8((Yq + m_rv * V) >> 16);
                dst += 3;
            }
        }
        return true;
    }
    case fg::PixelFormat::P010: {

        const uint8_t* yBase = f.data;
        const uint8_t* uvBase = f.data + pitch * f.height;
        for (int y = 0; y < H; ++y) {
            const uint8_t* yRow = yBase + (y0 + y) * pitch;
            const uint8_t* uvRow = uvBase + ((y0 + y) >> 1) * pitch;
            uint8_t* dst = out + static_cast<size_t>(y) * W * 3;
            for (int x = 0; x < W; ++x) {
                const int px = x0 + x;
                const int Yq = (yRow[px * 2 + 1] - m_yo) * m_ys;
                const uint8_t* uv = uvRow + (px & ~1) * 2;
                const int U = uv[1] - 128;
                const int V = uv[3] - 128;
                dst[0] = Clamp8((Yq + m_bu * U) >> 16);
                dst[1] = Clamp8((Yq - m_gu * U - m_gv * V) >> 16);
                dst[2] = Clamp8((Yq + m_rv * V) >> 16);
                dst += 3;
            }
        }
        return true;
    }
    case fg::PixelFormat::BGRA: {
        for (int y = 0; y < H; ++y) {
            const uint8_t* src = f.data + (y0 + y) * pitch +
                static_cast<ptrdiff_t>(x0) * 4;
            uint8_t* dst = out + static_cast<size_t>(y) * W * 3;
            for (int x = 0; x < W; ++x) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst += 3;
                src += 4;
            }
        }
        return true;
    }
    case fg::PixelFormat::RGB24: {

        for (int y = 0; y < H; ++y) {
            const uint8_t* src = f.data + (y0 + y) * pitch +
                static_cast<ptrdiff_t>(x0) * 3;
            std::memcpy(out + static_cast<size_t>(y) * W * 3, src,
                static_cast<size_t>(W) * 3);
        }
        return true;
    }
    default:
        return false;
    }
}
