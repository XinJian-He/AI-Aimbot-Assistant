#include "Capture.h"

#include <mfcaptureengine.h>

#include <cmath>
#include <stdexcept>

#define RET_HR(x)                                                        \
    do {                                                                 \
        const HRESULT _hr = (x);                                         \
        if (FAILED(_hr)) {                                               \
            return _hr;                                                  \
        }                                                                \
    } while (0)

const wchar_t* PixelFormatName(PixelFormat f)
{
    switch (f) {
    case PixelFormat::NV12: return L"NV12";
    case PixelFormat::YUY2: return L"YUY2";
    case PixelFormat::P010: return L"P010";
    case PixelFormat::BGRA: return L"RGB32";
    case PixelFormat::RGB24: return L"RGB24";
    case PixelFormat::MJPEG: return L"MJPG";
    default: return L"?";
    }
}

PixelFormat SubtypeToFormat(const GUID& s)
{
    if (s == MFVideoFormat_NV12) return PixelFormat::NV12;
    if (s == MFVideoFormat_YUY2) return PixelFormat::YUY2;
    if (s == MFVideoFormat_P010) return PixelFormat::P010;
    if (s == MFVideoFormat_RGB32 || s == MFVideoFormat_ARGB32) return PixelFormat::BGRA;
    if (s == MFVideoFormat_RGB24) return PixelFormat::RGB24;
    if (s == MFVideoFormat_MJPG) return PixelFormat::MJPEG;
    return PixelFormat::Unknown;
}

std::wstring ModeDesc(const VideoMode& m)
{
    wchar_t buf[128];
    swprintf_s(buf, L"%ux%u @ %.6g fps %s%s",
               m.width, m.height, m.Fps(), PixelFormatName(m.format),
               m.progressive ? L"" : L" (隔行)");
    return buf;
}

std::vector<DeviceInfo> EnumVideoDevices()
{
    std::vector<DeviceInfo> out;
    ComPtr<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (FAILED(hr))throw std::runtime_error("MFCreateAttributes failed: "+std::to_string(static_cast<unsigned long>(hr)));
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attr.Get(), &acts, &count);
    if (FAILED(hr))throw std::runtime_error("MFEnumDeviceSources failed: "+std::to_string(static_cast<unsigned long>(hr)));

    for (UINT32 i = 0; i < count; ++i) {
        DeviceInfo d;
        d.activate.Attach(acts[i]);
        WCHAR* s = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(acts[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &len))) {
            d.name = s;
            CoTaskMemFree(s);
        }
        if (SUCCEEDED(acts[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &len))) {
            d.symlink = s;
            CoTaskMemFree(s);
        }
        out.push_back(std::move(d));
    }
    CoTaskMemFree(acts);
    return out;
}

static HRESULT GetVideoTypeHandler(IMFMediaSource* source, ComPtr<IMFMediaTypeHandler>& handler)
{
    ComPtr<IMFPresentationDescriptor> pd;
    RET_HR(source->CreatePresentationDescriptor(&pd));
    DWORD count = 0;
    pd->GetStreamDescriptorCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        if (FAILED(pd->GetStreamDescriptorByIndex(i, &selected, &sd)))
            continue;
        ComPtr<IMFMediaTypeHandler> h;
        if (FAILED(sd->GetMediaTypeHandler(&h)))
            continue;
        GUID major{};
        if (FAILED(h->GetMajorType(&major)) || major != MFMediaType_Video)
            continue;
        handler = h;
        return S_OK;
    }
    return MF_E_NOT_FOUND;
}

std::vector<VideoMode> EnumModes(IMFMediaSource* source)
{
    std::vector<VideoMode> out;
    ComPtr<IMFMediaTypeHandler> h;
    if (FAILED(GetVideoTypeHandler(source, h)))
        return out;

    DWORD n = 0;
    h->GetMediaTypeCount(&n);

    for (DWORD k = 0; k < n; ++k) {
        ComPtr<IMFMediaType> t;
        if (FAILED(h->GetMediaTypeByIndex(k, &t)))
            continue;
        GUID sub{};
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)))
            continue;

        UINT32 w = 0, hh = 0, num = 0, den = 1;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &hh);
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        const UINT32 il = MFGetAttributeUINT32(t.Get(), MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        const PixelFormat f = SubtypeToFormat(sub);
        if (f == PixelFormat::Unknown || !w || !hh || !num)
            continue;

        VideoMode m;
        m.format = f;
        m.subtype = sub;
        m.nativeType = t;
        m.width = w;
        m.height = hh;
        m.fpsNum = num;
        m.fpsDen = den;
        m.progressive = (il == MFVideoInterlace_Progressive || il == MFVideoInterlace_Unknown);

        bool dup = false;
        for (const auto& e : out) {
            if (e.subtype == m.subtype && e.width == m.width && e.height == m.height &&
                e.fpsNum == m.fpsNum && e.fpsDen == m.fpsDen) {
                dup = true;
                break;
            }
        }
        if (!dup)
            out.push_back(std::move(m));
    }
    return out;
}

int PickBestMode(const std::vector<VideoMode>& modes,
                 UINT32 wantW, UINT32 wantH, double wantFps, PixelFormat wantFmt)
{
    int best = -1;
    double bestScore = -1e300;
    for (int i = 0; i < (int)modes.size(); ++i) {
        const VideoMode& m = modes[i];
        if (wantW && m.width != wantW) continue;
        if (wantH && m.height != wantH) continue;
        if (wantFps > 0 && std::fabs(m.Fps() - wantFps) > 0.005) continue;
        if (wantFmt != PixelFormat::Unknown && m.format != wantFmt) continue;

        double score = 0;
        if (m.progressive) score += 1e18;
        if (m.format != PixelFormat::MJPEG) score += 1e17;
        if (wantFps > 0 && std::fabs(m.Fps() - wantFps) < 0.005)
            score += 1e15;
        score += double(m.width) * m.height * m.Fps();
        score += m.Fps() * 1000.0;
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

STDMETHODIMP CaptureSession::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFSourceReaderCallback)) {
        *ppv = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CaptureSession::Release()
{
    const ULONG r = --m_refs;
    if (r == 0)
        delete this;
    return r;
}

HRESULT CaptureSession::Start(IMFMediaSource* source, const VideoMode& mode, HANDLE frameEvent)
{
    m_frameEvent = frameEvent;
    m_source = source;
    m_mjpegSource = (mode.format == PixelFormat::MJPEG);
    m_flushDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    {
        ComPtr<IMFMediaTypeHandler> h;
        RET_HR(GetVideoTypeHandler(source, h));
        RET_HR(h->SetCurrentMediaType(mode.nativeType.Get()));
    }

    ComPtr<IMFAttributes> attr;
    RET_HR(MFCreateAttributes(&attr, 6));
    attr->SetUINT32(MF_LOW_LATENCY, TRUE);
    attr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, m_mjpegSource ? FALSE : TRUE);
    attr->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback*>(this));

    attr->SetString(MF_READWRITE_MMCSS_CLASS, L"Capture");
    attr->SetUINT32(MF_READWRITE_MMCSS_PRIORITY, 1);
    RET_HR(MFCreateSourceReaderFromMediaSource(source, attr.Get(), &m_reader));

    m_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    RET_HR(m_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE));

    HRESULT hr = S_OK;
    if (m_mjpegSource) {

        static const GUID outFmts[] = { MFVideoFormat_NV12, MFVideoFormat_YUY2 };
        hr = E_FAIL;
        for (const GUID& g : outFmts) {
            ComPtr<IMFMediaType> out;
            if (FAILED(MFCreateMediaType(&out)))
                continue;
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out->SetGUID(MF_MT_SUBTYPE, g);
            MFSetAttributeSize(out.Get(),MF_MT_FRAME_SIZE,mode.width,mode.height);
            MFSetAttributeRatio(out.Get(),MF_MT_FRAME_RATE,mode.fpsNum,mode.fpsDen);
            hr = m_reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get());
            if (SUCCEEDED(hr))
                break;
        }
        RET_HR(hr);
    } else {

        RET_HR(m_reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mode.nativeType.Get()));
    }

    m_stopping=false;
    RefreshActiveType();

    RET_HR(m_reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr));
    return S_OK;
}

void CaptureSession::Stop()
{
    m_stopping = true;

    ComPtr<IMFSourceReader> reader;
    {
        std::lock_guard lk(m_lock);
        reader = m_reader;
    }
    if (reader) {

        if (SUCCEEDED(reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS)) && m_flushDone)
            WaitForSingleObject(m_flushDone, 2000);
    }
    {
        std::lock_guard lk(m_lock);
        m_reader.Reset();
        m_latest = {};
        m_hasFrame = false;
    }
    if (m_source) {
        m_source->Shutdown();
        m_source.Reset();
    }
    if (m_flushDone) {
        CloseHandle(m_flushDone);
        m_flushDone = nullptr;
    }
}

bool CaptureSession::AcquireLatest(Frame& out)
{
    std::lock_guard lk(m_lock);
    if (!m_hasFrame)
        return false;
    out = std::move(m_latest);
    m_latest = {};
    m_hasFrame = false;
    return true;
}

VideoMode CaptureSession::ActiveMode() const
{
    std::lock_guard lk(m_lock);
    return m_active;
}

void CaptureSession::RefreshActiveType()
{
    ComPtr<IMFSourceReader> reader;
    {
        std::lock_guard lk(m_lock);
        reader = m_reader;
    }
    if (!reader)
        return;
    ComPtr<IMFMediaType> t;
    if (FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t)))
        return;

    VideoMode m;
    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &m.width, &m.height);
    MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &m.fpsNum, &m.fpsDen);
    t->GetGUID(MF_MT_SUBTYPE, &m.subtype);
    m.format = SubtypeToFormat(m.subtype);
    const UINT32 il = MFGetAttributeUINT32(t.Get(), MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    m.progressive = (il == MFVideoInterlace_Progressive || il == MFVideoInterlace_Unknown);
    m.nominalRange = MFGetAttributeUINT32(t.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Unknown);
    m.yuvMatrix = MFGetAttributeUINT32(t.Get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_Unknown);
    m.nativeType = t;

    LONG stride = (LONG)MFGetAttributeUINT32(t.Get(), MF_MT_DEFAULT_STRIDE, 0);
    if (stride == 0) {
        LONG tmp = 0;
        if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(m.subtype.Data1, m.width, &tmp)) && tmp != 0)
            stride = tmp;
    }
    m_stride = stride;

    {
        std::lock_guard lk(m_lock);
        m_active = m;
    }
}

STDMETHODIMP CaptureSession::OnReadSample(HRESULT hrStatus, DWORD, DWORD streamFlags,
                                          LONGLONG timestamp, IMFSample* sample)
{
    if (m_stopping)
        return S_OK;

    if (FAILED(hrStatus)) {
        m_lastError = hrStatus;
        m_streamError = true;
        if (m_frameEvent)
            SetEvent(m_frameEvent);
        return S_OK;
    }
    if (streamFlags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) {
        m_streamError = true;
        if (m_frameEvent)
            SetEvent(m_frameEvent);
        return S_OK;
    }
    if (streamFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
        RefreshActiveType();

    {
        ComPtr<IMFSourceReader> reader;
        {
            std::lock_guard lk(m_lock);
            reader = m_reader;
        }
        if (reader && !m_stopping)
            reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    }

    if (sample) {
        Frame f;
        f.sample = sample;
        f.mfTimestamp = timestamp;
        f.arrivalQpc = MFGetSystemTime();
        UINT64 dev = 0;
        if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DeviceReferenceSystemTime, &dev)) && dev)
            f.deviceQpc = (LONGLONG)dev;

        {
            std::lock_guard lk(m_lock);
            if (m_hasFrame)
                m_drops.fetch_add(1, std::memory_order_relaxed);
            f.index = m_frames.fetch_add(1, std::memory_order_relaxed) + 1;
            m_latest = std::move(f);
            m_hasFrame = true;
        }
        if (m_frameEvent)
            SetEvent(m_frameEvent);
    }
    return S_OK;
}

STDMETHODIMP CaptureSession::OnFlush(DWORD)
{
    if (m_flushDone)
        SetEvent(m_flushDone);
    return S_OK;
}
