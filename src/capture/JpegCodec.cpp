#include "JpegCodec.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

namespace jpeg {
namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

IWICImagingFactory* CreateFactory() {
    IWICImagingFactory* f = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return SUCCEEDED(hr) ? f : nullptr;
}

}

void CenterBlitBgr24(const uint8_t* src, int srcW, int srcH,
    uint8_t* dst, int dstW, int dstH) {
    if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        return;
    }
    const int copyW = std::min(srcW, dstW);
    const int copyH = std::min(srcH, dstH);
    if (copyW != dstW || copyH != dstH) {
        std::memset(dst, 0, static_cast<size_t>(dstW) * dstH * 3);
    }
    const size_t srcPitch = static_cast<size_t>(srcW) * 3;
    const size_t dstPitch = static_cast<size_t>(dstW) * 3;
    const uint8_t* s = src
        + static_cast<size_t>((srcH - copyH) / 2) * srcPitch
        + static_cast<size_t>((srcW - copyW) / 2) * 3;
    uint8_t* d = dst
        + static_cast<size_t>((dstH - copyH) / 2) * dstPitch
        + static_cast<size_t>((dstW - copyW) / 2) * 3;
    for (int y = 0; y < copyH; ++y) {
        std::memcpy(d, s, static_cast<size_t>(copyW) * 3);
        s += srcPitch;
        d += dstPitch;
    }
}

Decoder::~Decoder() {
    SafeRelease(m_factory);
}

IWICImagingFactory* Decoder::Factory() {
    if (!m_factory) {
        m_factory = CreateFactory();
    }
    return m_factory;
}

bool Decoder::Decode(const uint8_t* data, uint32_t size,
    std::vector<uint8_t>& bgr, int& width, int& height) {
    IWICImagingFactory* factory = Factory();
    if (!factory || !data || size < 4) {
        return false;
    }

    IStream* mem = SHCreateMemStream(data, size);
    if (!mem) {
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;

    do {
        if (FAILED(factory->CreateDecoderFromStream(mem, nullptr,
                WICDecodeMetadataCacheOnDemand, &decoder))) {
            break;
        }
        if (FAILED(decoder->GetFrame(0, &frame))) {
            break;
        }
        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 ||
            w > 16384 || h > 16384) {
            break;
        }
        if (FAILED(factory->CreateFormatConverter(&converter))) {
            break;
        }
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat24bppBGR,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom))) {
            break;
        }
        const UINT pitch = w * 3;
        bgr.resize(static_cast<size_t>(pitch) * h);
        if (FAILED(converter->CopyPixels(nullptr, pitch,
                static_cast<UINT>(bgr.size()), bgr.data()))) {
            break;
        }
        width = static_cast<int>(w);
        height = static_cast<int>(h);
        ok = true;
    } while (false);

    SafeRelease(converter);
    SafeRelease(frame);
    SafeRelease(decoder);
    mem->Release();
    return ok;
}

Encoder::~Encoder() {
    SafeRelease(m_factory);
}

IWICImagingFactory* Encoder::Factory() {
    if (!m_factory) {
        m_factory = CreateFactory();
    }
    return m_factory;
}

bool Encoder::Encode(const uint8_t* bgr, int width, int height,
    int quality, std::vector<uint8_t>& out) {
    IWICImagingFactory* factory = Factory();
    if (!factory || !bgr || width <= 0 || height <= 0) {
        return false;
    }

    IStream* mem = SHCreateMemStream(nullptr, 0);
    if (!mem) {
        return false;
    }

    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = false;

    do {
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr,
                &encoder))) {
            break;
        }
        if (FAILED(encoder->Initialize(mem, WICBitmapEncoderNoCache))) {
            break;
        }
        if (FAILED(encoder->CreateNewFrame(&frame, &props))) {
            break;
        }
        if (props) {
            PROPBAG2 opt{};
            wchar_t name[] = L"ImageQuality";
            opt.pstrName = name;
            VARIANT v{};
            v.vt = VT_R4;
            v.fltVal = std::clamp(quality, 1, 100) / 100.0f;
            props->Write(1, &opt, &v);
        }
        if (FAILED(frame->Initialize(props))) {
            break;
        }
        if (FAILED(frame->SetSize(static_cast<UINT>(width),
                static_cast<UINT>(height)))) {
            break;
        }
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        if (FAILED(frame->SetPixelFormat(&fmt)) ||
            !IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR)) {
            break;
        }
        const UINT pitch = static_cast<UINT>(width) * 3;
        if (FAILED(frame->WritePixels(static_cast<UINT>(height), pitch,
                pitch * static_cast<UINT>(height),
                const_cast<BYTE*>(bgr)))) {
            break;
        }
        if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
            break;
        }

        STATSTG st{};
        if (FAILED(mem->Stat(&st, STATFLAG_NONAME))) {
            break;
        }
        const ULONG len = static_cast<ULONG>(st.cbSize.QuadPart);
        out.resize(len);
        LARGE_INTEGER zero{};
        if (FAILED(mem->Seek(zero, STREAM_SEEK_SET, nullptr))) {
            break;
        }
        ULONG read = 0;
        if (FAILED(mem->Read(out.data(), len, &read)) || read != len) {
            break;
        }
        ok = true;
    } while (false);

    SafeRelease(props);
    SafeRelease(frame);
    SafeRelease(encoder);
    mem->Release();
    return ok;
}

}
