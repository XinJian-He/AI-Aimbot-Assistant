#pragma once

#include "Common.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

enum class PixelFormat { NV12, YUY2, P010, BGRA, RGB24, MJPEG, Unknown };

const wchar_t* PixelFormatName(PixelFormat f);
PixelFormat SubtypeToFormat(const GUID& subtype);

struct VideoMode
{
    UINT32 width = 0, height = 0;
    UINT32 fpsNum = 0, fpsDen = 1;
    PixelFormat format = PixelFormat::Unknown;
    GUID subtype{};
    bool progressive = true;
    UINT32 nominalRange = 0;
    UINT32 yuvMatrix = 0;
    ComPtr<IMFMediaType> nativeType;

    double Fps() const { return fpsDen ? double(fpsNum) / double(fpsDen) : 0.0; }
    bool SameSignal(const VideoMode& o) const
    {
        return width == o.width && height == o.height && subtype == o.subtype;
    }
};

struct DeviceInfo
{
    std::wstring name;
    std::wstring symlink;
    ComPtr<IMFActivate> activate;
};

std::vector<DeviceInfo> EnumVideoDevices();
std::vector<VideoMode> EnumModes(IMFMediaSource* source);

int PickBestMode(const std::vector<VideoMode>& modes,
                 UINT32 wantW, UINT32 wantH, double wantFps, PixelFormat wantFmt);

std::wstring ModeDesc(const VideoMode& m);

struct Frame
{
    ComPtr<IMFSample> sample;
    uint64_t index = 0;
    LONGLONG mfTimestamp = 0;
    LONGLONG deviceQpc = 0;
    LONGLONG arrivalQpc = 0;
};

class CaptureSession final : public IMFSourceReaderCallback
{
public:
    CaptureSession() = default;

    HRESULT Start(IMFMediaSource* source, const VideoMode& mode, HANDLE frameEvent);
    void Stop();

    bool AcquireLatest(Frame& out);
    VideoMode ActiveMode() const;
    LONG Stride() const { return m_stride.load(std::memory_order_relaxed); }
    bool SourceIsMjpeg() const { return m_mjpegSource; }

    uint64_t FramesCaptured() const { return m_frames.load(std::memory_order_relaxed); }
    uint64_t FramesDropped() const { return m_drops.load(std::memory_order_relaxed); }
    bool StreamFailed() const { return m_streamError.load(std::memory_order_relaxed); }
    HRESULT LastError() const { return m_lastError.load(std::memory_order_relaxed); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD streamIndex, DWORD streamFlags,
                              LONGLONG timestamp, IMFSample* sample) override;
    STDMETHODIMP OnFlush(DWORD streamIndex) override;
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    ~CaptureSession() = default;

    void RefreshActiveType();

    std::atomic<ULONG> m_refs{ 1 };
    ComPtr<IMFMediaSource> m_source;
    ComPtr<IMFSourceReader> m_reader;
    HANDLE m_frameEvent = nullptr;
    HANDLE m_flushDone = nullptr;
    bool m_mjpegSource = false;

    mutable std::mutex m_lock;
    Frame m_latest;
    bool m_hasFrame = false;
    VideoMode m_active;

    std::atomic<LONG> m_stride{ 0 };
    std::atomic<uint64_t> m_frames{ 0 }, m_drops{ 0 };
    std::atomic<bool> m_streamError{ false };
    std::atomic<HRESULT> m_lastError{ S_OK };
    std::atomic<bool> m_stopping{ false };
};
