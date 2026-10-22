#pragma once

#include <cstdint>

namespace stream {

constexpr uint32_t kHelloMagic = 0x484D4941;
constexpr uint32_t kFrameMagic = 0x464D4941;

constexpr uint32_t kFormatRaw = 0;
constexpr uint32_t kFormatMjpeg = 1;

constexpr uint32_t kMaxPayload = 64u * 1024 * 1024;

#pragma pack(push, 1)
struct StreamHello {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
};

struct StreamFrameHeader {
    uint32_t magic;
    uint32_t seq;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    int64_t presentQpc;
    int64_t sendQpc;
    int64_t qpcFreq;
    uint32_t dataLen;
};
#pragma pack(pop)

}
