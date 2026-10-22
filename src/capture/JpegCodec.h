#pragma once

#include <cstdint>
#include <vector>

struct IWICImagingFactory;

namespace jpeg {

void CenterBlitBgr24(const uint8_t* src, int srcW, int srcH,
    uint8_t* dst, int dstW, int dstH);

class Decoder {
public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool Decode(const uint8_t* data, uint32_t size,
        std::vector<uint8_t>& bgr, int& width, int& height);

private:
    IWICImagingFactory* Factory();
    IWICImagingFactory* m_factory = nullptr;
};

class Encoder {
public:
    Encoder() = default;
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    bool Encode(const uint8_t* bgr, int width, int height,
        int quality, std::vector<uint8_t>& out);

private:
    IWICImagingFactory* Factory();
    IWICImagingFactory* m_factory = nullptr;
};

}
