#include "FrameGrabber.h"

#include "Capture.h"
#include "Common.h"
#include "DShowCapture.h"
#include "DeviceIdentity.h"
#include <stdexcept>

#include <cstdlib>
#include <cmath>
#include <cwctype>
#include <mutex>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace fg {

namespace {

PixelFormat FromInternal(::PixelFormat f)
{
    switch (f) {
    case ::PixelFormat::NV12: return PixelFormat::NV12;
    case ::PixelFormat::YUY2: return PixelFormat::YUY2;
    case ::PixelFormat::P010: return PixelFormat::P010;
    case ::PixelFormat::BGRA: return PixelFormat::BGRA;
    case ::PixelFormat::RGB24: return PixelFormat::RGB24;
    case ::PixelFormat::MJPEG: return PixelFormat::MJPEG;
    default: return PixelFormat::Unknown;
    }
}

::PixelFormat ToInternal(PixelFormat f)
{
    switch (f) {
    case PixelFormat::NV12: return ::PixelFormat::NV12;
    case PixelFormat::YUY2: return ::PixelFormat::YUY2;
    case PixelFormat::P010: return ::PixelFormat::P010;
    case PixelFormat::BGRA: return ::PixelFormat::BGRA;
    case PixelFormat::RGB24: return ::PixelFormat::RGB24;
    case PixelFormat::MJPEG: return ::PixelFormat::MJPEG;
    default: return ::PixelFormat::Unknown;
    }
}

void EnsureInit()
{
    static std::once_flag once;
    std::call_once(once, [] {
        // Capture and enumeration callers own COM initialization on their thread.
        const HRESULT hr=MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if(FAILED(hr))throw std::runtime_error("Media Foundation startup failed (HRESULT="+std::to_string(static_cast<unsigned long>(hr))+")");
    });
}

std::wstring Lower(std::wstring s)
{
    for (auto& c : s)
        c = (wchar_t)towlower(c);
    return s;
}

int PickDeviceIndex(const std::vector<::DeviceInfo>& devs, const std::wstring& want)
{
    if (devs.empty())
        return -1;
    if (!want.empty()) {
        for (int i = 0; i < (int)devs.size(); ++i)
            if (SameDeviceId(devs[i].symlink, want))
                return i;
        const bool numeric = std::all_of(want.begin(), want.end(),
                                         [](wchar_t c) { return iswdigit(c) != 0; });
        if (numeric) {
            const int idx = _wtoi(want.c_str());
            return (idx >= 0 && idx < (int)devs.size()) ? idx : -1;
        }
        const std::wstring w = Lower(want);
        for (int i = 0; i < (int)devs.size(); ++i)
            if (Lower(devs[i].name).find(w) != std::wstring::npos)
                return i;
        return -1;
    }
    int best = 0, bestScore = -1;
    for (int i = 0; i < (int)devs.size(); ++i) {
        const std::wstring n = Lower(devs[i].name);
        int s = 0;
        if (n.find(L"gc553") != std::wstring::npos) s = 100;
        else if (n.find(L"live gamer") != std::wstring::npos) s = 90;
        else if (n.find(L"avermedia") != std::wstring::npos) s = 80;
        else if (n.find(L"capture") != std::wstring::npos) s = 50;
        if (s > bestScore) {
            bestScore = s;
            best = i;
        }
    }
    return best;
}

bool MatchesRequestedMode(const ModeDesc& mode, const Config& cfg) {
    // Mode keys carry three decimal places; tolerate serialization/100ns
    // rounding, but never accept 59.94 as 60 or 60 as 180.
    return (!cfg.width || mode.width == cfg.width) &&
        (!cfg.height || mode.height == cfg.height) &&
        (!(cfg.fps > 0) || (std::isfinite(mode.fps) && std::fabs(mode.fps - cfg.fps) <= 0.005));
}

HRESULT ActivateSource(::DeviceInfo& dev, ComPtr<IMFMediaSource>& src)
{
    HRESULT hr = dev.activate->ActivateObject(IID_PPV_ARGS(&src));
    if (SUCCEEDED(hr))
        return hr;

    dev.activate->ShutdownObject();
    src.Reset();
    Sleep(200);
    return dev.activate->ActivateObject(IID_PPV_ARGS(&src));
}

LONG DefaultPitch(const VideoMode& m)
{
    switch (m.format) {
    case ::PixelFormat::NV12: return (LONG)m.width;
    case ::PixelFormat::P010: return (LONG)(m.width * 2);
    case ::PixelFormat::YUY2: return (LONG)(m.width * 2);
    case ::PixelFormat::BGRA: return (LONG)(m.width * 4);
    case ::PixelFormat::RGB24: return (LONG)((m.width * 3 + 3) & ~3u);
    default: return (LONG)m.width;
    }
}

ModeDesc ToModeDesc(const VideoMode& m)
{
    ModeDesc d{ m.width, m.height, m.Fps(), FromInternal(m.format) };

    if (m.yuvMatrix == MFVideoTransferMatrix_BT709)
        d.bt709 = true;
    else if (m.yuvMatrix == MFVideoTransferMatrix_BT601 && m.height < 720)
        d.bt709 = false;
    else
        d.bt709 = (m.height >= 720);
    d.fullRange = (m.nominalRange == MFNominalRange_0_255);
    return d;
}

}

const wchar_t* PixelFormatName(PixelFormat f)
{
    return ::PixelFormatName(ToInternal(f));
}

int64_t Now100ns()
{
    return (int64_t)QpcNow100ns();
}

struct Frame::Lock
{
    ::Frame inner;
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMF2DBuffer2> b2;
    bool locked2d = false, locked = false;

    std::shared_ptr<std::vector<uint8_t>> dshowHold;

    ~Lock()
    {
        if (locked2d)
            b2->Unlock2D();
        else if (locked)
            buf->Unlock();
    }
};

Frame::Frame() = default;
Frame::~Frame() = default;
Frame::Frame(Frame&&) noexcept = default;
Frame& Frame::operator=(Frame&&) noexcept = default;

void Frame::Release()
{
    m_lock.reset();
    data = nullptr;
    pitch = 0;
}

struct Grabber::Impl
{
    mutable std::mutex mtx;
    ComPtr<CaptureSession> session;
    DeviceDesc activeDev;
    bool mjpegSource = false;

    DShowCapture dshow;
    bool usingDshow = false;

    HANDLE evt = nullptr;

    ComPtr<CaptureSession> Snapshot() const
    {
        std::lock_guard lk(mtx);
        return session;
    }

    bool OnDshow() const
    {
        std::lock_guard lk(mtx);
        return usingDshow;
    }

    ~Impl()
    {
        if (evt)
            CloseHandle(evt);
    }
};

Grabber::Grabber() : m(std::make_unique<Impl>())
{
    m->evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Grabber::~Grabber()
{
    Stop();
}

std::vector<DeviceDesc> Grabber::EnumDevices()
{
    std::vector<DeviceDesc> out;
    std::string mfError,dsError;
    try {EnsureInit();for(const auto& d : ::EnumVideoDevices())if(!d.symlink.empty())out.push_back({d.name,d.symlink});}
    catch(const std::exception& e){mfError=e.what();}
    // MF may expose an internal camera while a capture card is DirectShow-only.
    // Merge both enumerations; never treat a nonempty MF result as exhaustive.
    try {for(const auto& d : DShowCapture::EnumDevices()){
        if(d.id.empty())continue;
        if(std::none_of(out.begin(),out.end(),[&](const auto& old){return SameDeviceId(old.id,d.id);}))out.push_back(d);
    }}catch(const std::exception& e){dsError=e.what();}
    if(out.empty()&&(!mfError.empty()||!dsError.empty()))throw std::runtime_error(mfError+" "+dsError);
    return out;
}

std::vector<ModeDesc> Grabber::EnumModes(const std::wstring& device)
{
    std::vector<ModeDesc> out;
    std::vector<::DeviceInfo> devs;
    try {EnsureInit();devs=::EnumVideoDevices();}catch(...){return DShowCapture::EnumModes(device);}
    const int idx = PickDeviceIndex(devs, device);
    if (idx >= 0) {
        ComPtr<IMFMediaSource> src;
        if (SUCCEEDED(ActivateSource(devs[idx], src))) {
            for (const auto& mode : ::EnumModes(src.Get()))
                out.push_back(ToModeDesc(mode));
            src->Shutdown();
            devs[idx].activate->ShutdownObject();
        }
    }

    if (out.empty())
        out = DShowCapture::EnumModes(device);
    return out;
}

bool Grabber::Start(const Config& cfg, std::wstring* err)
{
    Stop();
    if(m->evt)ResetEvent(m->evt);
    std::wstring mfErr;
    auto fail = [&](const std::wstring& why) {
        mfErr = why;
        return false;
    };

    auto fallback = [&]() -> bool {
        Stop();
        std::wstring dsErr;
        if (m->dshow.Start(cfg, m->evt, &dsErr) && MatchesRequestedMode(m->dshow.ActiveMode(), cfg)) {
            std::lock_guard lk(m->mtx);
            m->usingDshow = true;
            m->activeDev = m->dshow.ActiveDevice();
            m->mjpegSource = m->dshow.SourceIsMjpeg();
            return true;
        }
        if (dsErr.empty()) dsErr = L"设备实际分辨率/帧率与所选格式不一致";
        m->dshow.Stop();
        if (err)
            *err = mfErr.empty() ? dsErr : (mfErr + L"; DirectShow 也失败: " + dsErr);
        return false;
    };

    std::vector<::DeviceInfo> devs;
    try {EnsureInit();devs=::EnumVideoDevices();}
    catch(...) {fail(L"Media Foundation 初始化/设备枚举失败");return fallback();}
    if (devs.empty()) {
        fail(L"MF 未找到任何视频采集设备");
        return fallback();
    }
    const int devIdx = PickDeviceIndex(devs, cfg.device);
    if (devIdx < 0) {
        fail(L"没有匹配 \"" + cfg.device + L"\" 的采集设备");
        return fallback();
    }

    ComPtr<IMFMediaSource> src;
    HRESULT hr = ActivateSource(devs[devIdx], src);
    if (FAILED(hr)) {
        src.Reset();devs[devIdx].activate->ShutdownObject();
        fail(L"打开设备失败, " + HrText(hr));
        return fallback();
    }

    auto modes = ::EnumModes(src.Get());
    const int mi = ::PickBestMode(modes, cfg.width, cfg.height, cfg.fps, ToInternal(cfg.format));
    if (mi < 0) {
        src->Shutdown();src.Reset();devs[devIdx].activate->ShutdownObject();
        fail(L"该设备没有匹配筛选条件的视频模式");
        return fallback();
    }

    ComPtr<CaptureSession> s;
    s.Attach(new CaptureSession());
    hr = s->Start(src.Get(), modes[mi], m->evt);
    if (FAILED(hr)) {
        s->Stop();s.Reset();
        src->Shutdown();src.Reset();devs[devIdx].activate->ShutdownObject();
        fail(L"开流失败, " + HrText(hr));
        return fallback();
    }

    if (!MatchesRequestedMode(ToModeDesc(s->ActiveMode()), cfg)) {
        s->Stop();s.Reset();
        src->Shutdown();src.Reset();devs[devIdx].activate->ShutdownObject();
        fail(L"MF 实际分辨率/帧率与所选格式不一致");
        return fallback();
    }

    ComPtr<CaptureSession> old;
    {
        std::lock_guard lk(m->mtx);
        old = m->session;
        m->session = s;
        m->usingDshow = false;
        m->activeDev = { devs[devIdx].name, devs[devIdx].symlink };
        m->mjpegSource = (modes[mi].format == ::PixelFormat::MJPEG);
    }
    if (old)
        old->Stop();
    return true;
}

void Grabber::Stop()
{
    ComPtr<CaptureSession> s;
    bool wasDshow = false;
    {
        std::lock_guard lk(m->mtx);
        s = m->session;
        wasDshow = m->usingDshow;
        m->session.Reset();
        m->usingDshow = false;
        m->activeDev = {};
        m->mjpegSource = false;
    }
    if (s)
        s->Stop();
    if (wasDshow)
        m->dshow.Stop();
}

bool Grabber::Running() const
{
    return m->Snapshot() != nullptr || m->OnDshow();
}

bool Grabber::WaitFrame(uint32_t timeoutMs) const
{
    return m->evt && WaitForSingleObject(m->evt, timeoutMs) == WAIT_OBJECT_0;
}

bool Grabber::AcquireLatest(Frame& out)
{
    out.Release();

    if (m->OnDshow()) {
        DShowCapture::Sample s;
        if (!m->dshow.AcquireLatest(s))
            return false;
        const ModeDesc dm = m->dshow.ActiveMode();

        auto lk = std::make_unique<Frame::Lock>();
        lk->dshowHold = std::move(s.hold);
        out.data = s.data;
        out.pitch = s.pitch;
        out.width = dm.width;
        out.height = dm.height;
        out.format = dm.format;
        out.timestamp100ns = s.timestamp100ns;
        out.index = s.index;
        out.m_lock = std::move(lk);
        return true;
    }

    ComPtr<CaptureSession> session = m->Snapshot();
    if (!session)
        return false;

    ::Frame f;
    if (!session->AcquireLatest(f))
        return false;

    const VideoMode am = session->ActiveMode();
    const int64_t ts = f.deviceQpc ? f.deviceQpc : f.arrivalQpc;

    auto lk = std::make_unique<Frame::Lock>();
    DWORD cnt = 0;
    f.sample->GetBufferCount(&cnt);

    HRESULT hr = (cnt <= 1) ? f.sample->GetBufferByIndex(0, &lk->buf)
                            : f.sample->ConvertToContiguousBuffer(&lk->buf);
    if (FAILED(hr) || !lk->buf)
        return false;

    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    if (SUCCEEDED(lk->buf.As(&lk->b2))) {
        BYTE* start = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(lk->b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &len)))
            lk->locked2d = true;
    }
    if (!lk->locked2d) {
        BYTE* raw = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(lk->buf->Lock(&raw, &maxLen, &curLen)))
            return false;
        lk->locked = true;
        pitch = session->Stride() ? session->Stride() : DefaultPitch(am);

        scan0 = (pitch >= 0) ? raw : raw + (size_t)(-pitch) * (am.height - 1);
    }

    lk->inner = std::move(f);
    out.data = scan0;
    out.pitch = pitch;
    out.width = am.width;
    out.height = am.height;
    out.format = FromInternal(am.format);
    out.timestamp100ns = ts;
    out.index = lk->inner.index; // identity of this sample, not a racing global counter
    out.m_lock = std::move(lk);
    return true;
}

ModeDesc Grabber::ActiveMode() const
{
    if (m->OnDshow())
        return m->dshow.ActiveMode();
    auto session = m->Snapshot();
    return session ? ToModeDesc(session->ActiveMode()) : ModeDesc{};
}

DeviceDesc Grabber::ActiveDevice() const
{
    std::lock_guard lk(m->mtx);
    return m->activeDev;
}

bool Grabber::SourceIsMjpeg() const
{
    std::lock_guard lk(m->mtx);
    return m->mjpegSource;
}

uint64_t Grabber::FramesCaptured() const
{
    if (m->OnDshow())
        return m->dshow.FramesCaptured();
    auto session = m->Snapshot();
    return session ? session->FramesCaptured() : 0;
}

uint64_t Grabber::FramesMerged() const
{
    if (m->OnDshow())
        return m->dshow.FramesDropped();
    auto session = m->Snapshot();
    return session ? session->FramesDropped() : 0;
}

bool Grabber::SignalLost() const
{
    auto session = m->Snapshot();
    return session ? session->StreamFailed() : false;
}

}
