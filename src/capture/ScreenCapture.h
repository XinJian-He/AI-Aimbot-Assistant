#ifndef SCREENCAPTURE_H
#define SCREENCAPTURE_H

#include "GDIScreenCapture.h"
#include "DXScreenCapture.h"
#include "WGCScreenCapture.h"
#include "ObsCapture.h"
#include "CardCapture.h"
#include "StreamCapture.h"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <Windows.h>

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    bool Init(int width, int height, int mode, const std::string& device = "",
        const std::string& cardMode = "", int udpPort = 7788, int streamPort = 9339);
    bool SetWindow(HWND hwnd);
    bool SetRegion(int x, int y, int width, int height);
    void Reset();
    std::vector<uint8_t> Capture();
    bool Capture(uint8_t* buffer, size_t bufferSize, UINT timeoutMs = 50);

    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
    int GetChannels() const { return channels; }
    const char* GetLastError() const;

private:
    void Release();

    std::unique_ptr<GDIScreenCapture> gdiCapture;
    std::unique_ptr<DXScreenCapture> dxCapture;
    std::unique_ptr<WGCScreenCapture> wgcCapture;

    std::unique_ptr<ObsCapture> obsCapture;
    std::unique_ptr<CardCapture> cardCapture;
    std::unique_ptr<StreamCapture> streamCapture;
    std::string error;

    int width;
    int height;
    int channels;
    float captureTime;
    bool initialized;
    int method; // 0=GDI, 1=DXGI, 2=WGC, 3=UDP, 4=Card, 5=Stream
};

#endif
