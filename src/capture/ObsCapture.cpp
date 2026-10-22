#include <WinSock2.h>

#include "ObsCapture.h"

#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "JpegCodec.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Synchronization.lib")

namespace {

constexpr size_t kAccumMax = 8u * 1024 * 1024;
constexpr size_t kAccumKeep = 2u * 1024 * 1024;
constexpr uint8_t kSoi[2] = {0xFF, 0xD8};
constexpr uint8_t kEoi[2] = {0xFF, 0xD9};

}

ObsCapture::~ObsCapture() {
    Reset();
}

bool ObsCapture::Init(int width, int height, int port) {
    Reset();
    if (width <= 0 || height <= 0 || port <= 0 || port > 65535) {
        return false;
    }
    m_outW = width;
    m_outH = height;
    m_port = port;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
    m_wsaInit = true;

    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        Reset();
        return false;
    }
    m_sock = static_cast<UINT_PTR>(s);

    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF,
        reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    DWORD timeout = 250;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
        Reset(); return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        Reset();
        return false;
    }

    m_latest.assign(static_cast<size_t>(width) * height * 3, 0);
    m_seq.store(0, std::memory_order_release);
    m_consumed = 0;
    m_lastQpc.store(0, std::memory_order_release);

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&ObsCapture::RecvLoop, this);
    return true;
}

void ObsCapture::Reset() {
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
    if (m_sock != ~static_cast<UINT_PTR>(0)) {
        closesocket(static_cast<SOCKET>(m_sock));
        m_sock = ~static_cast<UINT_PTR>(0);
    }
    if (m_wsaInit) {
        WSACleanup();
        m_wsaInit = false;
    }
    m_outW = 0;
    m_outH = 0;
    m_port = 0;
    m_consumed = 0;
    m_latest.clear();
}

bool ObsCapture::Capture(uint8_t* buffer, size_t bufferSize, UINT timeoutMs) {
    const size_t need = static_cast<size_t>(m_outW) * m_outH * 3;
    if (!buffer || need == 0 || bufferSize < need) {
        return false;
    }
    uint32_t cur = m_seq.load(std::memory_order_acquire);
    if (cur == m_consumed) {
        uint32_t undesired = cur;
        WaitOnAddress(reinterpret_cast<volatile VOID*>(&m_seq),
            &undesired, sizeof(undesired), timeoutMs);
        cur = m_seq.load(std::memory_order_acquire);
        if (cur == m_consumed) {
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::memcpy(buffer, m_latest.data(), need);
    }
    m_consumed = cur;
    return true;
}

std::string ObsCapture::DescUtf8() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf),
        "OBS stream udp:%d  center %dx%d", m_port, m_outW, m_outH);
    return buf;
}

void ObsCapture::RecvLoop() {

    struct ComScope {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
    } com;

    jpeg::Decoder decoder;
    std::vector<uint8_t> accum;
    accum.reserve(1024 * 1024);
    std::vector<uint8_t> pkt(65536);
    std::vector<uint8_t> frameJpeg;
    std::vector<uint8_t> bgr;
    std::vector<uint8_t> out(static_cast<size_t>(m_outW) * m_outH * 3);
    bool announced = false;

    const SOCKET s = static_cast<SOCKET>(m_sock);
    while (m_running.load(std::memory_order_acquire)) {
        const int n = recvfrom(s, reinterpret_cast<char*>(pkt.data()),
            static_cast<int>(pkt.size()), 0, nullptr, nullptr);
        if (n <= 0) {
            continue;
        }
        accum.insert(accum.end(), pkt.begin(), pkt.begin() + n);
        if (accum.size() > kAccumMax) {
            accum.erase(accum.begin(), accum.end() - kAccumKeep);
        }

        const auto soi = std::find_end(
            accum.begin(), accum.end(), kSoi, kSoi + 2);
        if (soi == accum.end()) {
            if (accum.size() > 1024 * 1024) {
                accum.clear();
            }
            continue;
        }
        const auto eoi = std::search(soi + 2, accum.end(), kEoi, kEoi + 2);
        if (eoi == accum.end()) {
            accum.erase(accum.begin(), soi);
            continue;
        }

        frameJpeg.assign(soi, eoi + 2);
        accum.erase(accum.begin(), eoi + 2);

        int w = 0, h = 0;
        if (!decoder.Decode(frameJpeg.data(),
                static_cast<uint32_t>(frameJpeg.size()), bgr, w, h)) {
            continue;
        }
        jpeg::CenterBlitBgr24(bgr.data(), w, h, out.data(), m_outW, m_outH);

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::memcpy(m_latest.data(), out.data(), out.size());
        }
        m_lastQpc.store(qpc.QuadPart, std::memory_order_release);
        m_seq.fetch_add(1, std::memory_order_release);
        WakeByAddressAll(&m_seq);

        if (!announced) {
            announced = true;
        }
    }


}
