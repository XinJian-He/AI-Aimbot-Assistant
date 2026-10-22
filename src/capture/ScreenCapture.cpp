#include "ScreenCapture.h"
#include <chrono>

ScreenCapture::ScreenCapture()
    : width(0)
    , height(0)
    , channels(3)
    , captureTime(0.0f)
    , initialized(false)
    , method(1) {
}

ScreenCapture::~ScreenCapture() {
    Release();
}

bool ScreenCapture::Init(int width, int height, int mode, const std::string& device,
    const std::string& cardMode, int udpPort, int streamPort) {
    Release();
    method = mode;
    error.clear();
    if (width <= 0 || height <= 0) { error = "采集尺寸无效"; return false; }

    this->width = width;
    this->height = height;

    if (method == 0) {
        gdiCapture = std::make_unique<GDIScreenCapture>();
        if (gdiCapture->Initialize(width, height)) {
            initialized = true;
            return true;
        }
        gdiCapture.reset();
    }
    else if (method == 1) {
        dxCapture = std::make_unique<DXScreenCapture>();
        if (dxCapture->Initialize(width, height)) {
            initialized = true;
            return true;
        }
        dxCapture.reset();
    }
    else if (method == 2) {
        wgcCapture = std::make_unique<WGCScreenCapture>();
        if (wgcCapture->Initialize(width, height)) {
            initialized = true;
            return true;
        }
        wgcCapture.reset();
    }

    else if (method == 3) {
        obsCapture = std::make_unique<ObsCapture>();
        initialized = obsCapture->Init(width, height, udpPort);
        if (!initialized) { error = "UDP 监听失败，请检查端口是否被占用"; obsCapture.reset(); }
        return initialized;
    } else if (method == 4) {
        std::wstring wide;
        if (!device.empty()) {
            int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, device.data(), (int)device.size(), nullptr, 0);
            if (count <= 0) { error = "采集卡标识编码无效"; return false; }
            wide.resize(count);
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, device.data(), (int)device.size(), wide.data(), count);
        }
        cardCapture = std::make_unique<CardCapture>();
        initialized = cardCapture->Init(width, height, wide, cardMode);
        if (!initialized) { error = cardCapture->LastError(); cardCapture.reset(); }
        return initialized;
    } else if (method == 5) {
        streamCapture = std::make_unique<StreamCapture>();
        initialized = streamCapture->Init(width, height, streamPort);
        if (!initialized) { error = "串流监听失败，请检查 TCP 端口是否被占用"; streamCapture.reset(); }
        return initialized;
    }
    return false;
}

bool ScreenCapture::SetWindow(HWND hwnd) {
    if (!initialized) return false;

    switch (method) {
    case 0:
        return gdiCapture ? gdiCapture->SetWindow(hwnd) : false;
    case 1:
        return false;
    case 2:
        return wgcCapture ? wgcCapture->SetWindow(hwnd) : false;
    default:
        return false;
    }
}

bool ScreenCapture::SetRegion(int x, int y, int width, int height) {
    if (!initialized) return false;

    bool success = false;

    switch (method) {
    case 0:
        success = gdiCapture ? gdiCapture->SetRegion(x, y, width, height) : false;
        break;
    case 1:
        success = dxCapture ? dxCapture->SetRegion(x, y, width, height) : false;
        break;
    case 2:
        success = wgcCapture ? wgcCapture->SetRegion(x, y, width, height) : false;
        break;
    default:
        return false;
    }

    if (success) {
        this->width = width;
        this->height = height;
    }

    return success;
}

void ScreenCapture::Reset() {
    Release();
    initialized = false;
    width = 0;
    height = 0;
    captureTime = 0.0f;
}

std::vector<uint8_t> ScreenCapture::Capture() {
    if (!initialized) return {};
    std::vector<uint8_t> result(static_cast<size_t>(width) * height * 3);
    if (!Capture(result.data(), result.size(), 50)) return {};
    return result;
}

bool ScreenCapture::Capture(uint8_t* buffer, size_t bufferSize, UINT timeoutMs) {
    if (!initialized || !buffer) return false;
    if (obsCapture) return obsCapture->Capture(buffer, bufferSize, timeoutMs);
    if (cardCapture) return cardCapture->Capture(buffer, bufferSize, timeoutMs);
    if (streamCapture) return streamCapture->Capture(buffer, bufferSize, timeoutMs);

    switch (method) {
    case 0:
        return gdiCapture ? gdiCapture->CaptureBGR(buffer, bufferSize) : false;
    case 1:
        return dxCapture ? dxCapture->CaptureBGR(buffer, bufferSize) : false;
    case 2:
        return wgcCapture ? wgcCapture->CaptureBGR(buffer, bufferSize) : false;
    default:
        return false;
    }
}

const char* ScreenCapture::GetLastError() const {
    if (!error.empty()) return error.c_str();
    switch (method) {
    case 0: return gdiCapture ? gdiCapture->GetLastError() : "GDI 未初始化";
    case 1: return dxCapture ? dxCapture->GetLastError() : "DXGI 未初始化";
    case 2: return wgcCapture ? wgcCapture->GetLastError() : "WGC 未初始化";
    default: return "未知后端";
    }
}

void ScreenCapture::Release() {
    initialized = false;
    obsCapture.reset();
    cardCapture.reset();
    streamCapture.reset();
    if (gdiCapture) {
        gdiCapture->Release();
        gdiCapture.reset();
    }

    if (dxCapture) {
        dxCapture->Release();
        dxCapture.reset();
    }

    if (wgcCapture) {
        wgcCapture->Release();
        wgcCapture.reset();
    }
}
