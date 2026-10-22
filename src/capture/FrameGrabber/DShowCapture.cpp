#include "DShowCapture.h"

#include "Common.h"
#include "DeviceIdentity.h"
#include <stdexcept>
#include "../JpegCodec.h"

#include <dshow.h>
#include <dvdmedia.h>

#include <cmath>
#include <functional>
#include <mutex>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace fg {

namespace {

constexpr DWORD Fcc(char a, char b, char c, char d)
{
    return (DWORD)(BYTE)a | ((DWORD)(BYTE)b << 8) | ((DWORD)(BYTE)c << 16) |
           ((DWORD)(BYTE)d << 24);
}

bool IsFourCcSubtype(const GUID& g)
{
    static const GUID kBase = { 0x00000000, 0x0000, 0x0010,
        { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
    return g.Data2 == kBase.Data2 && g.Data3 == kBase.Data3 &&
           memcmp(g.Data4, kBase.Data4, sizeof(g.Data4)) == 0;
}

PixelFormat FormatFromSubtype(const GUID& g)
{
    if (IsEqualGUID(g, MEDIASUBTYPE_RGB24))
        return PixelFormat::RGB24;
    if (IsEqualGUID(g, MEDIASUBTYPE_RGB32) || IsEqualGUID(g, MEDIASUBTYPE_ARGB32))
        return PixelFormat::BGRA;
    if (!IsFourCcSubtype(g))
        return PixelFormat::Unknown;
    switch (g.Data1) {
    case Fcc('Y', 'U', 'Y', '2'): return PixelFormat::YUY2;
    case Fcc('N', 'V', '1', '2'): return PixelFormat::NV12;
    case Fcc('P', '0', '1', '0'): return PixelFormat::P010;
    case Fcc('M', 'J', 'P', 'G'): return PixelFormat::MJPEG;
    default: return PixelFormat::Unknown;
    }
}

int32_t StrideOf(PixelFormat f, int32_t width)
{
    switch (f) {
    case PixelFormat::NV12: return width;
    case PixelFormat::YUY2: return width * 2;
    case PixelFormat::P010: return width * 2;
    case PixelFormat::BGRA: return width * 4;
    case PixelFormat::RGB24: return (width * 3 + 3) & ~3;
    default: return width;
    }
}

size_t FrameBytes(PixelFormat f, int32_t width, int32_t height)
{
    const size_t stride = (size_t)StrideOf(f, width);
    switch (f) {
    case PixelFormat::NV12: return stride * height * 3 / 2;
    case PixelFormat::P010: return stride * height * 3 / 2;
    default: return stride * (size_t)height;
    }
}

void ClearMediaType(AM_MEDIA_TYPE* mt)
{
    if (!mt)
        return;
    if (mt->cbFormat && mt->pbFormat)
        CoTaskMemFree(mt->pbFormat);
    if (mt->pUnk)
        mt->pUnk->Release();
    mt->pbFormat = nullptr;
    mt->cbFormat = 0;
    mt->pUnk = nullptr;
}

void DeleteMediaType(AM_MEDIA_TYPE* mt)
{
    if (!mt)
        return;
    ClearMediaType(mt);
    CoTaskMemFree(mt);
}

HRESULT CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src)
{
    *dst = *src;
    dst->pbFormat = nullptr;
    if (src->cbFormat && src->pbFormat) {
        dst->pbFormat = (BYTE*)CoTaskMemAlloc(src->cbFormat);
        if (!dst->pbFormat) {
            dst->cbFormat = 0;
            return E_OUTOFMEMORY;
        }
        memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    }
    if (dst->pUnk)
        dst->pUnk->AddRef();
    return S_OK;
}

struct VideoFormatInfo
{
    int32_t width = 0;
    int32_t height = 0;
    double fps = 0.0;
    bool bottomUp = false;
    bool valid = false;
};

VideoFormatInfo ReadVideoFormat(const AM_MEDIA_TYPE* mt)
{
    VideoFormatInfo v;
    const BITMAPINFOHEADER* bmi = nullptr;
    REFERENCE_TIME avg = 0;

    if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo) &&
        mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
        bmi = &vih->bmiHeader;
        avg = vih->AvgTimePerFrame;
    } else if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo2) &&
               mt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        auto* vih = reinterpret_cast<VIDEOINFOHEADER2*>(mt->pbFormat);
        bmi = &vih->bmiHeader;
        avg = vih->AvgTimePerFrame;
    }
    if (!bmi)
        return v;

    v.width = bmi->biWidth;
    v.height = bmi->biHeight < 0 ? -bmi->biHeight : bmi->biHeight;
    v.fps = avg > 0 ? 1e7 / (double)avg : 0.0;
    const PixelFormat f = FormatFromSubtype(mt->subtype);
    v.bottomUp = bmi->biHeight > 0 &&
                 (f == PixelFormat::RGB24 || f == PixelFormat::BGRA);
    v.valid = v.width > 0 && v.height > 0;
    return v;
}

void SetAvgTimePerFrame(AM_MEDIA_TYPE* mt, REFERENCE_TIME avg)
{
    if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo) &&
        mt->cbFormat >= sizeof(VIDEOINFOHEADER))
        reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat)->AvgTimePerFrame = avg;
    else if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo2) &&
             mt->cbFormat >= sizeof(VIDEOINFOHEADER2))
        reinterpret_cast<VIDEOINFOHEADER2*>(mt->pbFormat)->AvgTimePerFrame = avg;
}

std::wstring ReadProp(IPropertyBag* bag, const wchar_t* key)
{
    VARIANT v;
    VariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(bag->Read(key, &v, nullptr)) && v.vt == VT_BSTR && v.bstrVal)
        out = v.bstrVal;
    VariantClear(&v);
    return out;
}

struct MonikerEntry
{
    std::wstring name;
    std::wstring path;
    ComPtr<IMoniker> moniker;
};

std::vector<MonikerEntry> EnumMonikers()
{
    std::vector<MonikerEntry> out;
    ComPtr<ICreateDevEnum> devEnum;
    const HRESULT createHr=CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum));
    if (FAILED(createHr))
        throw std::runtime_error("DirectShow device enumerator failed: "+std::to_string(static_cast<unsigned long>(createHr)));

    ComPtr<IEnumMoniker> monikers;
    const HRESULT enumHr=devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers,0);
    if(FAILED(enumHr))throw std::runtime_error("DirectShow device enumeration failed: "+std::to_string(static_cast<unsigned long>(enumHr)));
    if (enumHr != S_OK || !monikers)
        return out;

    ComPtr<IMoniker> mon;
    while (monikers->Next(1, mon.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        MonikerEntry e;
        e.moniker = mon;
        ComPtr<IPropertyBag> bag;
        if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
            e.name = ReadProp(bag.Get(), L"FriendlyName");
            e.path = ReadProp(bag.Get(), L"DevicePath");
        }
        if(e.path.empty()){
            ComPtr<IBindCtx> context;LPOLESTR display=nullptr;
            if(SUCCEEDED(CreateBindCtx(0,&context))&&SUCCEEDED(mon->GetDisplayName(context.Get(),nullptr,&display))&&display){
                e.path=display;CoTaskMemFree(display);
            }
        }
        if(e.name.empty())e.name=e.path;
        if(!e.path.empty())out.push_back(std::move(e));
    }
    return out;
}

std::wstring Lower(std::wstring s)
{
    for (auto& c : s)
        c = (wchar_t)towlower(c);
    return s;
}

int PickDevice(const std::vector<MonikerEntry>& devs, const std::wstring& want)
{
    if (devs.empty())
        return -1;
    if (want.empty())
        return 0;
    for (int i = 0; i < (int)devs.size(); ++i)
        if (SameDeviceId(devs[i].path, want))
            return i;
    if (std::all_of(want.begin(), want.end(),
                    [](wchar_t c) { return iswdigit(c) != 0; })) {
        const int idx = _wtoi(want.c_str());
        return (idx >= 0 && idx < (int)devs.size()) ? idx : -1;
    }
    const std::wstring w = Lower(want);
    for (int i = 0; i < (int)devs.size(); ++i)
        if (Lower(devs[i].name).find(w) != std::wstring::npos)
            return i;
    return -1;
}

ComPtr<IPin> FindVideoCapturePin(IBaseFilter* filter)
{
    ComPtr<IEnumPins> pins;
    if (FAILED(filter->EnumPins(&pins)))
        return nullptr;

    ComPtr<IPin> fallback;
    ComPtr<IPin> pin;
    while (pins->Next(1, pin.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        PIN_DIRECTION dir{};
        if (FAILED(pin->QueryDirection(&dir)) || dir != PINDIR_OUTPUT)
            continue;

        ComPtr<IEnumMediaTypes> types;
        bool isVideo = false;
        if (SUCCEEDED(pin->EnumMediaTypes(&types)) && types) {
            AM_MEDIA_TYPE* mt = nullptr;
            if (types->Next(1, &mt, nullptr) == S_OK && mt) {
                isVideo = IsEqualGUID(mt->majortype, MEDIATYPE_Video) != FALSE;
                DeleteMediaType(mt);
            }
        }
        if (!isVideo)
            continue;

        ComPtr<IKsPropertySet> ks;
        if (SUCCEEDED(pin.As(&ks))) {
            GUID category{};
            DWORD got = 0;
            if (SUCCEEDED(ks->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY,
                                  nullptr, 0, &category, sizeof(category), &got)) &&
                IsEqualGUID(category, PIN_CATEGORY_CAPTURE))
                return pin;
        }
        if (!fallback)
            fallback = pin;
    }
    return fallback;
}

struct CapEntry
{
    int index = 0;
    ModeDesc mode;
    REFERENCE_TIME minInterval = 0;
    REFERENCE_TIME maxInterval = 0;
};

const double kFpsLadder[] = { 60.0,  75.0,  90.0,  100.0, 120.0, 144.0,
                              165.0, 180.0, 200.0, 240.0, 280.0, 300.0,
                              360.0, 390.0, 400.0, 480.0, 500.0, 540.0,
                              600.0 };

std::vector<CapEntry> EnumStreamCaps(IAMStreamConfig* cfg)
{
    std::vector<CapEntry> out;
    int count = 0, size = 0;
    if (FAILED(cfg->GetNumberOfCapabilities(&count, &size)) ||
        size != sizeof(VIDEO_STREAM_CONFIG_CAPS))
        return out;

    for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* mt = nullptr;
        VIDEO_STREAM_CONFIG_CAPS caps{};
        if (FAILED(cfg->GetStreamCaps(i, &mt, reinterpret_cast<BYTE*>(&caps))) || !mt)
            continue;

        const VideoFormatInfo v = ReadVideoFormat(mt);
        const PixelFormat f = FormatFromSubtype(mt->subtype);
        if (v.valid && f != PixelFormat::Unknown && v.fps > 0.0) {
            CapEntry base;
            base.index = i;
            base.mode.width = (uint32_t)v.width;
            base.mode.height = (uint32_t)v.height;
            base.mode.format = f;
            base.mode.bt709 = v.height >= 720;
            base.mode.fullRange = false;
            base.minInterval = caps.MinFrameInterval;
            base.maxInterval = caps.MaxFrameInterval;

            const double maxFps = caps.MinFrameInterval > 0
                                      ? 1e7 / (double)caps.MinFrameInterval
                                      : v.fps;
            const double minFps = caps.MaxFrameInterval > 0
                                      ? 1e7 / (double)caps.MaxFrameInterval
                                      : v.fps;
            auto push = [&](double fps) {
                for (const auto& e : out)
                    if (e.index == i && std::fabs(e.mode.fps - fps) < 0.6)
                        return;
                CapEntry e = base;
                e.mode.fps = fps;
                out.push_back(e);
            };

            push(v.fps);
            for (double f2 : kFpsLadder)
                if (f2 >= minFps - 0.6 && f2 <= maxFps + 0.6)
                    push(f2);
            push(maxFps);
        }
        DeleteMediaType(mt);
    }
    return out;
}

int PickBestCap(const std::vector<CapEntry>& caps, const Config& cfg)
{
    int best = -1;
    double bestScore = -1e300;
    for (int i = 0; i < (int)caps.size(); ++i) {
        const ModeDesc& m = caps[i].mode;
        if (cfg.width && m.width != cfg.width)
            continue;
        if (cfg.height && m.height != cfg.height)
            continue;
        if (cfg.format != PixelFormat::Unknown && m.format != cfg.format)
            continue;

        double fps = m.fps;
        if (cfg.fps > 0) {
            const double lo = caps[i].maxInterval > 0 ? 1e7 / (double)caps[i].maxInterval : m.fps;
            const double hi = caps[i].minInterval > 0 ? 1e7 / (double)caps[i].minInterval : m.fps;
            if (cfg.fps < lo - 0.6 || cfg.fps > hi + 0.6)
                continue;
            fps = cfg.fps;
        }

        double score = 0;
        if (m.format != PixelFormat::MJPEG)
            score += 1e17;
        if (cfg.fps > 0 && std::fabs(m.fps - cfg.fps) < 0.005)
            score += 1e15;
        score += (double)m.width * m.height * fps;
        score += fps * 1000.0;
        switch (m.format) {
        case PixelFormat::NV12: score += 400; break;
        case PixelFormat::YUY2: score += 300; break;
        case PixelFormat::P010: score += 200; break;
        case PixelFormat::BGRA: score += 100; break;
        default: break;
        }
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

void EnsureCom()
{
    // Balanced once per calling thread, including short-lived enumeration workers.
    struct Apartment {HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED|COINIT_DISABLE_OLE1DDE);
        ~Apartment(){if(SUCCEEDED(hr))CoUninitialize();}};
    thread_local Apartment apartment;
    if(FAILED(apartment.hr)&&apartment.hr!=RPC_E_CHANGED_MODE)throw std::runtime_error("DirectShow COM initialization failed");
}

class FrameSink;

class SinkPin final : public IPin, public IMemInputPin
{
public:
    explicit SinkPin(FrameSink* owner) : m_owner(owner) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP ReceiveConnection(IPin* peer, const AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** peer) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* d) override
    {
        *d = PINDIR_INPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR* id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG* n) override
    {
        if (n)
            *n = 0;
        return S_OK;
    }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override
    {
        return S_OK;
    }

    STDMETHODIMP GetAllocator(IMemAllocator**) override { return VFW_E_NO_ALLOCATOR; }
    STDMETHODIMP NotifyAllocator(IMemAllocator*, BOOL) override { return S_OK; }
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP Receive(IMediaSample* sample) override;
    STDMETHODIMP ReceiveMultiple(IMediaSample** samples, long n, long* done) override;
    STDMETHODIMP ReceiveCanBlock() override { return S_FALSE; }

    void ReleaseConnection()
    {
        m_peer.Reset();
        ClearMediaType(&m_mt);
    }

private:
    FrameSink* m_owner;
    ComPtr<IPin> m_peer;
    AM_MEDIA_TYPE m_mt{};
};

class PinEnum final : public IEnumPins
{
public:
    explicit PinEnum(IPin* pin) : m_pin(pin) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG r = --m_refs;
        if (!r)
            delete this;
        return r;
    }

    STDMETHODIMP Next(ULONG count, IPin** out, ULONG* fetched) override
    {
        ULONG n = 0;
        if (count > 0 && m_pos == 0) {
            out[0] = m_pin;
            out[0]->AddRef();
            m_pos = 1;
            n = 1;
        }
        if (fetched)
            *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override
    {
        m_pos += count;
        return m_pos > 1 ? S_FALSE : S_OK;
    }
    STDMETHODIMP Reset() override
    {
        m_pos = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins** out) override
    {
        auto* e = new PinEnum(m_pin);
        e->m_pos = m_pos;
        *out = e;
        return S_OK;
    }

private:
    ~PinEnum() = default;
    std::atomic<ULONG> m_refs{ 1 };
    IPin* m_pin;
    ULONG m_pos = 0;
};

class FrameSink final : public IBaseFilter
{
public:
    using OnSample = std::function<void(const uint8_t*, long)>;

    FrameSink(const AM_MEDIA_TYPE& accept, OnSample cb)
        : m_pin(this), m_onSample(std::move(cb))
    {
        CopyMediaType(&m_accept, &accept);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter ||
            riid == IID_IBaseFilter) {
            *ppv = static_cast<IBaseFilter*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG r = --m_refs;
        if (!r)
            delete this;
        return r;
    }

    STDMETHODIMP GetClassID(CLSID* c) override
    {
        *c = CLSID_NULL;
        return S_OK;
    }

    STDMETHODIMP Stop() override
    {
        m_state = State_Stopped;
        return S_OK;
    }
    STDMETHODIMP Pause() override
    {
        m_state = State_Paused;
        return S_OK;
    }
    STDMETHODIMP Run(REFERENCE_TIME) override
    {
        m_state = State_Running;
        return S_OK;
    }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* s) override
    {
        if (!s)
            return E_POINTER;
        *s = m_state;
        return S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock* c) override
    {
        m_clock = c;
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock** c) override
    {
        if (!c)
            return E_POINTER;
        *c = m_clock.Get();
        if (*c)
            (*c)->AddRef();
        return S_OK;
    }

    STDMETHODIMP EnumPins(IEnumPins** e) override
    {
        if (!e)
            return E_POINTER;
        *e = new PinEnum(static_cast<IPin*>(&m_pin));
        return S_OK;
    }
    STDMETHODIMP FindPin(LPCWSTR id, IPin** p) override
    {
        if (!p)
            return E_POINTER;
        if (id && wcscmp(id, L"In") == 0) {
            *p = static_cast<IPin*>(&m_pin);
            (*p)->AddRef();
            return S_OK;
        }
        *p = nullptr;
        return VFW_E_NOT_FOUND;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override
    {
        if (!info)
            return E_POINTER;
        wcscpy_s(info->achName, L"AiMod Sink");
        info->pGraph = m_graph;
        if (info->pGraph)
            info->pGraph->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* g, LPCWSTR) override
    {
        m_graph = g;
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR*) override { return E_NOTIMPL; }

    const AM_MEDIA_TYPE& Accepted() const { return m_accept; }
    IPin* Pin() { return static_cast<IPin*>(&m_pin); }
    void Deliver(const uint8_t* data, long len) { m_onSample(data, len); }

private:
    ~FrameSink() { ClearMediaType(&m_accept); }

    std::atomic<ULONG> m_refs{ 1 };
    SinkPin m_pin;
    OnSample m_onSample;
    AM_MEDIA_TYPE m_accept{};
    FILTER_STATE m_state = State_Stopped;
    ComPtr<IReferenceClock> m_clock;
    IFilterGraph* m_graph = nullptr;
};

STDMETHODIMP SinkPin::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == IID_IMemInputPin) {
        *ppv = static_cast<IMemInputPin*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) SinkPin::AddRef()
{
    return m_owner->AddRef();
}

STDMETHODIMP_(ULONG) SinkPin::Release()
{
    return m_owner->Release();
}

STDMETHODIMP SinkPin::ReceiveConnection(IPin* peer, const AM_MEDIA_TYPE* mt)
{
    if (!peer || !mt)
        return E_POINTER;
    if (m_peer)
        return VFW_E_ALREADY_CONNECTED;
    if (QueryAccept(mt) != S_OK)
        return VFW_E_TYPE_NOT_ACCEPTED;
    ClearMediaType(&m_mt);
    const HRESULT hr = CopyMediaType(&m_mt, mt);
    if (FAILED(hr))
        return hr;
    m_peer = peer;
    return S_OK;
}

STDMETHODIMP SinkPin::Disconnect()
{
    ReleaseConnection();
    return S_OK;
}

STDMETHODIMP SinkPin::ConnectedTo(IPin** peer)
{
    if (!peer)
        return E_POINTER;
    *peer = m_peer.Get();
    if (!*peer)
        return VFW_E_NOT_CONNECTED;
    (*peer)->AddRef();
    return S_OK;
}

STDMETHODIMP SinkPin::ConnectionMediaType(AM_MEDIA_TYPE* mt)
{
    if (!mt)
        return E_POINTER;
    if (!m_peer)
        return VFW_E_NOT_CONNECTED;
    return CopyMediaType(mt, &m_mt);
}

STDMETHODIMP SinkPin::QueryPinInfo(PIN_INFO* info)
{
    if (!info)
        return E_POINTER;
    info->pFilter = static_cast<IBaseFilter*>(m_owner);
    info->pFilter->AddRef();
    info->dir = PINDIR_INPUT;
    wcscpy_s(info->achName, L"In");
    return S_OK;
}

STDMETHODIMP SinkPin::QueryId(LPWSTR* id)
{
    if (!id)
        return E_POINTER;
    *id = (LPWSTR)CoTaskMemAlloc(sizeof(L"In"));
    if (!*id)
        return E_OUTOFMEMORY;
    wcscpy_s(*id, 3, L"In");
    return S_OK;
}

STDMETHODIMP SinkPin::QueryAccept(const AM_MEDIA_TYPE* mt)
{
    if (!mt)
        return E_POINTER;
    const AM_MEDIA_TYPE& want = m_owner->Accepted();
    if (!IsEqualGUID(mt->majortype, want.majortype) ||
        !IsEqualGUID(mt->subtype, want.subtype))
        return S_FALSE;
    const VideoFormatInfo a = ReadVideoFormat(mt);
    const VideoFormatInfo b = ReadVideoFormat(&want);
    if (!a.valid || a.width != b.width || a.height != b.height)
        return S_FALSE;
    return S_OK;
}

STDMETHODIMP SinkPin::Receive(IMediaSample* sample)
{
    if (!sample)
        return E_POINTER;
    BYTE* data = nullptr;
    if (FAILED(sample->GetPointer(&data)) || !data)
        return S_OK;
    const long len = sample->GetActualDataLength();
    if (len > 0)
        m_owner->Deliver(data, len);
    return S_OK;
}

STDMETHODIMP SinkPin::ReceiveMultiple(IMediaSample** samples, long n, long* done)
{
    long i = 0;
    for (; i < n; ++i) {
        const HRESULT hr = Receive(samples[i]);
        if (FAILED(hr))
            break;
    }
    if (done)
        *done = i;
    return S_OK;
}

}

struct DShowCapture::Impl
{
    mutable std::mutex mtx;

    ComPtr<IGraphBuilder> graph;
    ComPtr<IMediaControl> control;
    ComPtr<IBaseFilter> source;
    ComPtr<FrameSink> sink;

    DeviceDesc device;
    ModeDesc mode;
    int32_t pitch = 0;
    size_t expectBytes = 0;
    bool bottomUp = false;
    bool compressed=false;

    HANDLE frameEvent = nullptr;
    std::atomic<uint64_t> frames{ 0 };
    std::atomic<uint64_t> drops{ 0 };

    std::shared_ptr<std::vector<uint8_t>> latest;
    int64_t latestTs = 0;
    uint64_t latestIndex = 0;
    bool hasFrame = false;

    std::vector<std::shared_ptr<std::vector<uint8_t>>> pool;

    std::shared_ptr<std::vector<uint8_t>> TakeBuffer(size_t bytes)
    {
        for (auto& b : pool) {
            if (b.use_count() == 1 && b->size() == bytes)
                return b;
        }
        auto fresh = std::make_shared<std::vector<uint8_t>>(bytes);
        if (pool.size() < 4)
            pool.push_back(fresh);
        return fresh;
    }

    void OnSample(const uint8_t* data, long len)
    {
        if(!data||len<=0)return;
        std::vector<uint8_t> decoded;
        if(compressed){
            // Decoder lifetime and COM apartment both belong to the callback thread.
            EnsureCom();jpeg::Decoder decoder;int width=0,height=0;
            if(!decoder.Decode(data,static_cast<uint32_t>(len),decoded,width,height)||
                width!=static_cast<int>(mode.width)||height!=static_cast<int>(mode.height)){
                drops.fetch_add(1,std::memory_order_relaxed);return;
            }
            data=decoded.data();len=static_cast<long>(decoded.size());
        }
        if ((size_t)len < expectBytes) {
            drops.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const int64_t ts = (int64_t)QpcNow100ns();
        {
            std::lock_guard lk(mtx);
            auto buf = TakeBuffer(expectBytes);
            memcpy(buf->data(), data, expectBytes);
            if (hasFrame)
                drops.fetch_add(1, std::memory_order_relaxed);
            latest = std::move(buf);
            latestTs = ts;
            latestIndex = frames.fetch_add(1, std::memory_order_relaxed) + 1;
            hasFrame = true;
        }
        if (frameEvent)
            SetEvent(frameEvent);
    }

    void Teardown()
    {
        if (control)
            control->Stop();
        if (graph) {
            if (source)
                graph->RemoveFilter(source.Get());
            if (sink)
                graph->RemoveFilter(sink.Get());
        }
        control.Reset();
        source.Reset();
        sink.Reset();
        graph.Reset();
        {
            std::lock_guard lk(mtx);
            latest.reset();
            pool.clear();
            hasFrame = false;
        }
        device = {};
        mode = {};
        pitch = 0;
        expectBytes = 0;
        bottomUp = false;
        compressed = false;
    }
};

DShowCapture::DShowCapture() : m(std::make_unique<Impl>()) {}

DShowCapture::~DShowCapture()
{
    Stop();
}

std::vector<DeviceDesc> DShowCapture::EnumDevices()
{
    EnsureCom();
    std::vector<DeviceDesc> out;
    for (const auto& e : EnumMonikers())
        out.push_back({ e.name, e.path });
    return out;
}

std::vector<ModeDesc> DShowCapture::EnumModes(const std::wstring& device)
{
    EnsureCom();
    std::vector<ModeDesc> out;

    auto devs = EnumMonikers();
    const int idx = PickDevice(devs, device);
    if (idx < 0)
        return out;

    ComPtr<IBaseFilter> filter;
    if (FAILED(devs[idx].moniker->BindToObject(nullptr, nullptr,
                                               IID_PPV_ARGS(&filter))))
        return out;

    ComPtr<IPin> pin = FindVideoCapturePin(filter.Get());
    ComPtr<IAMStreamConfig> cfg;
    if (!pin || FAILED(pin.As(&cfg)))
        return out;

    for (const auto& c : EnumStreamCaps(cfg.Get()))
        out.push_back(c.mode);
    return out;
}

bool DShowCapture::Start(const Config& cfg, HANDLE frameEvent, std::wstring* err)
{
    Stop();
    EnsureCom();

    auto fail = [&](const std::wstring& why) {
        m->Teardown();
        if (err)
            *err = why;
        return false;
    };

    auto devs = EnumMonikers();
    if (devs.empty())
        return fail(L"DirectShow 下没有任何视频设备");
    const int idx = PickDevice(devs, cfg.device);
    if (idx < 0)
        return fail(L"没有匹配 \"" + cfg.device + L"\" 的采集设备");

    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&m->graph));
    if (FAILED(hr))
        return fail(L"创建过滤器图失败, " + HrText(hr));
    if (FAILED(m->graph.As(&m->control)))
        return fail(L"取 IMediaControl 失败");

    ComPtr<IMediaFilter> graphFilter;
    if (SUCCEEDED(m->graph.As(&graphFilter)))
        graphFilter->SetSyncSource(nullptr);

    hr = devs[idx].moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&m->source));
    if (FAILED(hr))
        return fail(L"打开设备失败, " + HrText(hr));
    hr = m->graph->AddFilter(m->source.Get(), L"Capture");
    if (FAILED(hr))
        return fail(L"把设备加入过滤器图失败, " + HrText(hr));

    ComPtr<IPin> outPin = FindVideoCapturePin(m->source.Get());
    ComPtr<IAMStreamConfig> streamCfg;
    if (!outPin || FAILED(outPin.As(&streamCfg)))
        return fail(L"设备没有可配置的视频采集 pin");

    const auto caps = EnumStreamCaps(streamCfg.Get());
    if (caps.empty())
        return fail(L"设备没报出任何可用的视频格式");
    const int pick = PickBestCap(caps, cfg);
    if (pick < 0)
        return fail(L"该设备没有匹配筛选条件的视频模式");

    AM_MEDIA_TYPE* chosen = nullptr;
    VIDEO_STREAM_CONFIG_CAPS scc{};
    hr = streamCfg->GetStreamCaps(caps[pick].index, &chosen,
                                  reinterpret_cast<BYTE*>(&scc));
    if (FAILED(hr) || !chosen)
        return fail(L"取回选中的视频格式失败, " + HrText(hr));

    const double wantFps = cfg.fps > 0.0 ? cfg.fps : caps[pick].mode.fps;
    if (wantFps > 0.0) {
        REFERENCE_TIME want = (REFERENCE_TIME)std::llround(1e7 / wantFps);
        if (scc.MinFrameInterval > 0 && want < scc.MinFrameInterval)
            want = scc.MinFrameInterval;
        if (scc.MaxFrameInterval > 0 && want > scc.MaxFrameInterval)
            want = scc.MaxFrameInterval;
        SetAvgTimePerFrame(chosen, want);
    }
    hr = streamCfg->SetFormat(chosen);
    DeleteMediaType(chosen);
    chosen = nullptr;
    if (FAILED(hr))
        return fail(L"设置视频格式失败, " + HrText(hr));

    AM_MEDIA_TYPE* actual = nullptr;
    hr = streamCfg->GetFormat(&actual);
    if (FAILED(hr) || !actual)
        return fail(L"读回视频格式失败, " + HrText(hr));

    const VideoFormatInfo vf = ReadVideoFormat(actual);
    const PixelFormat pf = FormatFromSubtype(actual->subtype);
    if (!vf.valid || pf == PixelFormat::Unknown) {
        DeleteMediaType(actual);
        return fail(L"设备生效的格式无法解析");
    }

    m->mode.width = (uint32_t)vf.width;
    m->mode.height = (uint32_t)vf.height;
    m->mode.fps = vf.fps;
    m->compressed=pf==PixelFormat::MJPEG;
    m->mode.format = m->compressed?PixelFormat::RGB24:pf;
    m->mode.bt709 = vf.height >= 720;
    m->mode.fullRange = false;
    m->bottomUp = !m->compressed && vf.bottomUp;
    m->expectBytes = m->compressed?static_cast<size_t>(vf.width)*vf.height*3:FrameBytes(pf,vf.width,vf.height);
    m->pitch = m->compressed?vf.width*3:StrideOf(pf,vf.width);
    m->device = { devs[idx].name, devs[idx].path };
    m->frameEvent = frameEvent;

    Impl* impl = m.get();
    m->sink.Attach(new FrameSink(*actual, [impl](const uint8_t* d, long n) {
        try{impl->OnSample(d,n);}catch(...){impl->drops.fetch_add(1,std::memory_order_relaxed);}
    }));
    hr = m->graph->AddFilter(m->sink.Get(), L"AiMod Sink");
    if (FAILED(hr)) {
        DeleteMediaType(actual);
        return fail(L"把接收器加入过滤器图失败, " + HrText(hr));
    }

    hr = m->graph->ConnectDirect(outPin.Get(), m->sink->Pin(), actual);
    DeleteMediaType(actual);
    if (FAILED(hr))
        return fail(L"连接采集 pin 失败, " + HrText(hr));

    hr = m->control->Run();
    if (FAILED(hr))
        return fail(L"启动过滤器图失败, " + HrText(hr));

    return true;
}

void DShowCapture::Stop()
{
    m->Teardown();
    m->frameEvent = nullptr;
    m->frames.store(0, std::memory_order_relaxed);
    m->drops.store(0, std::memory_order_relaxed);
}

bool DShowCapture::Running() const
{
    return m->control != nullptr;
}

bool DShowCapture::SourceIsMjpeg() const
{
    std::lock_guard lk(m->mtx);
    return m->compressed;
}

bool DShowCapture::AcquireLatest(Sample& out)
{
    std::lock_guard lk(m->mtx);
    if (!m->hasFrame || !m->latest)
        return false;
    out.hold = m->latest;
    out.timestamp100ns = m->latestTs;
    out.index = m->latestIndex;

    const uint8_t* base = out.hold->data();
    if (m->bottomUp) {
        out.pitch = -m->pitch;
        out.data = base + (size_t)m->pitch * (m->mode.height - 1);
    } else {
        out.pitch = m->pitch;
        out.data = base;
    }
    m->hasFrame = false;
    return true;
}

ModeDesc DShowCapture::ActiveMode() const
{
    std::lock_guard lk(m->mtx);
    return m->mode;
}

DeviceDesc DShowCapture::ActiveDevice() const
{
    std::lock_guard lk(m->mtx);
    return m->device;
}

uint64_t DShowCapture::FramesCaptured() const
{
    return m->frames.load(std::memory_order_relaxed);
}

uint64_t DShowCapture::FramesDropped() const
{
    return m->drops.load(std::memory_order_relaxed);
}

}
