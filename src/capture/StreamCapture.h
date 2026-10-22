#ifndef STREAM_CAPTURE_H
#define STREAM_CAPTURE_H

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class StreamCapture {
public:
    StreamCapture() = default;
    ~StreamCapture();
    StreamCapture(const StreamCapture&) = delete;
    StreamCapture& operator=(const StreamCapture&) = delete;

    bool Init(int width, int height, int port);
    void Reset();

    bool Capture(uint8_t* buffer, size_t bufferSize, UINT timeoutMs);

    LONGLONG GetLastPresentQpc() const {
        return m_lastQpc.load(std::memory_order_acquire);
    }

    int GetWidth() const { return m_outW; }
    int GetHeight() const { return m_outH; }
    int GetChannels() const { return 3; }

    std::string DescUtf8() const;

private:
    void ServeLoop();
    bool RecvExact(UINT_PTR sock, uint8_t* dst, size_t len);
    bool SendAll(UINT_PTR sock, const void* data, size_t len);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    UINT_PTR m_listen = ~static_cast<UINT_PTR>(0);
    bool m_wsaInit = false;
    int m_outW = 0;
    int m_outH = 0;
    int m_port = 0;

    std::mutex m_mutex;
    std::vector<uint8_t> m_latest;
    std::atomic<uint32_t> m_seq{0};
    uint32_t m_consumed = 0;
    std::atomic<LONGLONG> m_lastQpc{0};
};

#endif
