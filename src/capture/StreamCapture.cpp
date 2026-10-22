#include <WinSock2.h>
#include <ws2tcpip.h>

#include "StreamCapture.h"

#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "JpegCodec.h"
#include "StreamProtocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Synchronization.lib")

StreamCapture::~StreamCapture() {
    Reset();
}

bool StreamCapture::Init(int width, int height, int port) {
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

    const SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        Reset();
        return false;
    }
    m_listen = static_cast<UINT_PTR>(ls);

    BOOL reuse = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(ls, 1) != 0) {
        Reset();
        return false;
    }

    m_latest.assign(static_cast<size_t>(width) * height * 3, 0);
    m_seq.store(0, std::memory_order_release);
    m_consumed = 0;
    m_lastQpc.store(0, std::memory_order_release);

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&StreamCapture::ServeLoop, this);
    return true;
}

void StreamCapture::Reset() {
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
    if (m_listen != ~static_cast<UINT_PTR>(0)) {
        closesocket(static_cast<SOCKET>(m_listen));
        m_listen = ~static_cast<UINT_PTR>(0);
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

bool StreamCapture::Capture(uint8_t* buffer, size_t bufferSize,
    UINT timeoutMs) {
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

std::string StreamCapture::DescUtf8() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf),
        "Stream RX tcp:%d  center %dx%d", m_port, m_outW, m_outH);
    return buf;
}

bool StreamCapture::RecvExact(UINT_PTR sockUp, uint8_t* dst, size_t len) {
    const SOCKET s = static_cast<SOCKET>(sockUp);
    size_t got = 0;
    while (got < len && m_running.load(std::memory_order_acquire)) {
        const int chunk = static_cast<int>(
            std::min<size_t>(len - got, 1u << 20));
        const int n = recv(s, reinterpret_cast<char*>(dst) + got, chunk, 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (WSAGetLastError() == WSAETIMEDOUT) {
            continue;
        }
        return false;
    }
    return got == len;
}

bool StreamCapture::SendAll(UINT_PTR sockUp, const void* data, size_t len) {
    const SOCKET s = static_cast<SOCKET>(sockUp);
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len && m_running.load(std::memory_order_acquire)) {
        const int n = send(s, p + sent,
            static_cast<int>(std::min<size_t>(len - sent, 1u << 20)), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return sent == len;
}

void StreamCapture::ServeLoop() {

    struct ComScope {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
    } com;

    jpeg::Decoder decoder;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> bgr;
    std::vector<uint8_t> out(static_cast<size_t>(m_outW) * m_outH * 3);

    const SOCKET ls = static_cast<SOCKET>(m_listen);
    while (m_running.load(std::memory_order_acquire)) {

        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(ls, &rs);
        timeval tv{0, 250 * 1000};
        const int sel = select(0, &rs, nullptr, nullptr, &tv);
        if (sel <= 0) {
            continue;
        }
        sockaddr_in peer{};
        int plen = sizeof(peer);
        const SOCKET c = accept(ls,
            reinterpret_cast<sockaddr*>(&peer), &plen);
        if (c == INVALID_SOCKET) {
            continue;
        }

        BOOL nodelay = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        int rcvbuf = 8 * 1024 * 1024;
        setsockopt(c, SOL_SOCKET, SO_RCVBUF,
            reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
        DWORD timeout = 250;
        if (setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0 ||
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO,
                reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
            closesocket(c); continue;
        }

        stream::StreamHello hello{stream::kHelloMagic,
            static_cast<uint32_t>(m_outW), static_cast<uint32_t>(m_outH)};
        if (!SendAll(static_cast<UINT_PTR>(c), &hello, sizeof(hello))) {
            closesocket(c);
            continue;
        }

        while (m_running.load(std::memory_order_acquire)) {
            stream::StreamFrameHeader hd{};
            if (!RecvExact(static_cast<UINT_PTR>(c),
                    reinterpret_cast<uint8_t*>(&hd), sizeof(hd))) {
                break;
            }
            if (hd.magic != stream::kFrameMagic || hd.dataLen == 0 ||
                hd.dataLen > stream::kMaxPayload ||
                hd.width == 0 || hd.width > 16384 ||
                hd.height == 0 || hd.height > 16384) {
                break;
            }
            payload.resize(hd.dataLen);
            if (!RecvExact(static_cast<UINT_PTR>(c),
                    payload.data(), hd.dataLen)) {
                break;
            }

            const uint8_t* src = nullptr;
            int sw = static_cast<int>(hd.width);
            int sh = static_cast<int>(hd.height);
            if (hd.format == stream::kFormatRaw) {
                if (static_cast<size_t>(sw) * sh * 3 != hd.dataLen) {
                    continue;
                }
                src = payload.data();
            } else if (hd.format == stream::kFormatMjpeg) {
                if (!decoder.Decode(payload.data(), hd.dataLen, bgr, sw, sh)) {
                    continue;
                }
                src = bgr.data();
            } else {
                continue;
            }

            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (sw == m_outW && sh == m_outH) {
                    std::memcpy(m_latest.data(), src, m_latest.size());
                } else {

                    jpeg::CenterBlitBgr24(src, sw, sh,
                        m_latest.data(), m_outW, m_outH);
                }
            }
            m_lastQpc.store(qpc.QuadPart, std::memory_order_release);
            m_seq.fetch_add(1, std::memory_order_release);
            WakeByAddressAll(&m_seq);
        }
        closesocket(c);
    }


}
