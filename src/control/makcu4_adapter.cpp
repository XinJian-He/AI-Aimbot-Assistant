#include "makcu_provider.h"
#if MAKCU_ENABLE_SCHEME4
#include "makcu4.h"
#include "makcu.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <algorithm>
#include <winreg.h>

extern void PushLog(const std::string& s);   // 定义在 AimBackend.cpp

namespace makcu_provider::legacy {
void makcu_close();
void makcu_release_all();
const char* makcu_get_last_error();

static HANDLE   g_makcu_hCom = INVALID_HANDLE_VALUE;
static std::atomic<bool> g_makcu_connected{ false };
static std::string g_makcu_lastError;
// Lifecycle excludes concurrent connect/close; I/O excludes handle replacement
// during a send/read. Never join the reader while holding the I/O lock.
static std::recursive_mutex g_makcu_lifecycleMtx;
static std::recursive_mutex g_makcu_writeMtx;
static std::mutex g_makcu_errorMtx;
static std::atomic<unsigned> g_makcu_moveLogged{ 0 };

// ── 接收抽干线程 ─────────────────────────────────────────────
// 持续读取提示符/可选回显，避免接收缓冲堆积。V3/V4 支持 echo 配置，
// 不依赖盒子此前保存的 echo 状态；错误响应保留在日志中。
static std::thread                     g_makcu_rxThread;
static std::atomic<bool>               g_makcu_rxStop{ false };
static std::atomic<unsigned long long> g_makcu_rxTotal{ 0 };   // 抽干字节数（诊断用）

static void makcu_set_error(const char* msg) {
    {
        std::lock_guard<std::mutex> lock(g_makcu_errorMtx);
        g_makcu_lastError = msg;
    }
    PushLog(std::string("[MAKCU] ") + msg);
}

static void makcu_io_error(const char* operation, DWORD code) {
    char message[256];
    snprintf(message, sizeof(message), "%s（Windows 错误码: %lu），请重新连接设备", operation, code);
    makcu_set_error(message);
    // Publish the reason before the backend observes the disconnection.
    g_makcu_rxStop.store(true);
    g_makcu_connected.store(false);
}

static bool makcu_read(char* buffer, DWORD capacity, DWORD& got) {
    std::lock_guard<std::recursive_mutex> lock(g_makcu_writeMtx);
    got = 0;
    if (g_makcu_hCom == INVALID_HANDLE_VALUE) return false;
    if (!ReadFile(g_makcu_hCom, buffer, capacity, &got, nullptr)) {
        makcu_io_error("串口读取失败", GetLastError());
        return false;
    }
    return true;
}

static bool makcu_line_looks_like_error(const std::string& line) {
    // 回显的命令行（km.move(...) 等）不含这些词；固件报错才会出现
    for (const char* kw : { "err", "ERR", "unknown", "UNKNOWN", "fail", "FAIL" })
        if (line.find(kw) != std::string::npos) return true;
    return false;
}

static void makcu_rx_loop() {
    // 句柄在线程存活期间固定：关句柄/换端口前都会先 makcu_rx_stop() join 本线程
    std::string line;
    int errLogged = 0;
    char buf[256];
    while (!g_makcu_rxStop.load(std::memory_order_acquire)) {
        DWORD got = 0;
        if (!makcu_read(buf, sizeof(buf), got)) break;
        if (got > 0) {
            g_makcu_rxTotal.fetch_add(got, std::memory_order_relaxed);
            for (DWORD i = 0; i < got; ++i) {
                char c = buf[i];
                if (c == '\n') {
                    if (!line.empty() && errLogged < 5 && makcu_line_looks_like_error(line)) {
                        errLogged++;
                        char b[256];
                        snprintf(b, sizeof(b), "[MAKCU] 设备返回: %s", line.c_str());
                        PushLog(b);
                    }
                    line.clear();
                } else if (c != '\r') {
                    if (line.size() < 200) line.push_back(c);
                    else line.clear();
                }
            }
            continue;
        }
        Sleep(5);
    }
}

static void makcu_rx_stop() {
    g_makcu_rxStop.store(true, std::memory_order_release);
    if (g_makcu_rxThread.joinable()) g_makcu_rxThread.join();
}

static void makcu_rx_start() {
    makcu_rx_stop();
    if (g_makcu_hCom == INVALID_HANDLE_VALUE) return;
    g_makcu_rxTotal.store(0, std::memory_order_relaxed);
    g_makcu_rxStop.store(false, std::memory_order_release);
    try {
        g_makcu_rxThread = std::thread(makcu_rx_loop);
    } catch (...) {
        g_makcu_connected.store(false);
        g_makcu_rxStop.store(true);
        throw;
    }
}

static bool makcu_write_raw(const char* s, bool handshaking = false) {
    std::lock_guard<std::recursive_mutex> lk(g_makcu_writeMtx);
    if (g_makcu_hCom == INVALID_HANDLE_VALUE || (!handshaking && !g_makcu_connected.load()))
        return false;
    const DWORD len = (DWORD)strlen(s);
    DWORD total = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (total < len) {
        DWORD written = 0;
        if (!WriteFile(g_makcu_hCom, s + total, len - total, &written, nullptr)) {
            makcu_io_error("串口写入失败", GetLastError());
            return false;
        }
        total += written;
        if (written == 0 || (total < len && std::chrono::steady_clock::now() >= deadline)) {
            // Never append another command to an incomplete command after a timeout.
            makcu_io_error("串口命令未完整发送", ERROR_TIMEOUT);
            return false;
        }
    }
    return true;
}

static bool makcu_port_error(const char* operation, const char* portName) {
    const DWORD code = GetLastError();
    char message[256];
    snprintf(message, sizeof(message), "%s %s（Windows 错误码: %lu）", operation, portName, code);
    makcu_set_error(message);
    if (g_makcu_hCom != INVALID_HANDLE_VALUE) CloseHandle(g_makcu_hCom);
    g_makcu_hCom = INVALID_HANDLE_VALUE;
    g_makcu_connected = false;
    return false;
}

static bool makcu_open_port(const char* portName, int baudRate) {
    makcu_rx_stop();   // 先停读线程再关句柄，避免读已关闭的句柄
    std::lock_guard<std::recursive_mutex> lock(g_makcu_writeMtx);
    if (g_makcu_hCom != INVALID_HANDLE_VALUE) {
        CloseHandle(g_makcu_hCom);
        g_makcu_hCom = INVALID_HANDLE_VALUE;
    }
    g_makcu_connected = false;

    if (!portName || !portName[0] || baudRate <= 0) {
        makcu_set_error("串口名不能为空，波特率必须为正整数");
        return false;
    }

    char fullPath[64];
    if (portName[0] != '\\') {
        snprintf(fullPath, sizeof(fullPath), "\\\\.\\%s", portName);
    } else {
        strncpy(fullPath, portName, sizeof(fullPath) - 1);
        fullPath[sizeof(fullPath) - 1] = '\0';
    }

    g_makcu_hCom = CreateFileA(fullPath, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (g_makcu_hCom == INVALID_HANDLE_VALUE)
        return makcu_port_error("无法打开串口", portName);

    // 与 K4HVH/makcu-cpp 的 Windows DCB 一致，禁用硬件/软件流控。
    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(g_makcu_hCom, &dcb))
        return makcu_port_error("无法读取串口设置", portName);
    dcb.BaudRate = baudRate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(g_makcu_hCom, &dcb))
        return makcu_port_error("无法设置串口参数", portName);

    // 同步串口句柄的读写会互相等待。使用 Windows 的立即返回模式，
    // 不让空闲 ReadFile 占用句柄 50ms，阻塞高频移动写入。
    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 50;
    if (!SetCommTimeouts(g_makcu_hCom, &timeouts))
        return makcu_port_error("无法设置串口超时", portName);

    PurgeComm(g_makcu_hCom, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

static bool makcu_read_until_prompt(std::string& out, int timeoutMs) {
    std::string acc;
    char tmp[128];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD got = 0;
        if (!makcu_read(tmp, sizeof(tmp), got)) { out = acc; return false; }
        if (got > 0) {
            acc.append(tmp, got);
            // A stale prompt or echoed command can precede the actual reply.
            // Require the identity followed by a prompt, including across reads.
            const auto identity = acc.find("MAKCU");
            if (identity != std::string::npos && acc.find(">>>", identity) != std::string::npos) {
                out = acc;
                return true;
            }
            if (acc.size() > 4096) acc.erase(0, acc.size() - 4096);
        } else {
            Sleep(5);
        }
    }
    out = acc;
    return false;
}

static bool makcu_handshake() {
    if (!makcu_write_raw("km.version()\r\n", true)) return false;
    std::string resp;
    if (!makcu_read_until_prompt(resp, 500))
        return false;
    PushLog("[MAKCU] 版本响应: " + resp);
    return true;
}

static std::vector<std::string> enumerate_com_ports() {
    std::vector<std::string> ports;
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0,
                      KEY_READ, &hKey) == ERROR_SUCCESS) {
        for (int i = 0; ; i++) {
            char valueName[256], valueData[256];
            DWORD nameLen = sizeof(valueName), dataLen = sizeof(valueData), type = 0;
            if (RegEnumValueA(hKey, i, valueName, &nameLen, nullptr, &type,
                              (BYTE*)valueData, &dataLen) != ERROR_SUCCESS)
                break;
            if (type == REG_SZ && valueData[0])
                ports.push_back(valueData);
        }
        RegCloseKey(hKey);
    }
    if (ports.empty()) {
        for (int i = 1; i <= 16; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "COM%d", i);
            ports.push_back(buf);
        }
    }
    return ports;
}

bool makcu_init(const char* portName, int baudRate) {
    std::lock_guard<std::recursive_mutex> lock(g_makcu_lifecycleMtx);
    makcu_close();
    { std::lock_guard<std::mutex> errorLock(g_makcu_errorMtx); g_makcu_lastError.clear(); }

    if (!makcu_open_port(portName, baudRate)) return false;

    if (!makcu_handshake()) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "握手失败（未识别到官方 MAKCU 固件，%s），请确认盒子固件为官方 ASCII 固件",
            portName);
        if (!makcu_get_last_error()[0]) makcu_set_error(buf);
        makcu_close();
        return false;
    }

    g_makcu_connected.store(true);
    g_makcu_moveLogged.store(0);
    makcu_rx_start();   // 握手完成后才开始抽干，避免抢走握手应答

    char buf[160];
    snprintf(buf, sizeof(buf), "串口握手成功 (%s, %d baud)；USB 鼠标输出效果需实机验证", portName, baudRate);
    PushLog(std::string("[MAKCU] ") + buf);
    return true;
}

bool makcu_autodetect(char* outPort, int* outBaud) {
    std::lock_guard<std::recursive_mutex> lock(g_makcu_lifecycleMtx);
    makcu_close();

    auto ports = enumerate_com_ports();
    // 自动扫描常用档位；其他速率可在配置中指定。设备速率可能持久保存，
    // SetCommState 只调整主机，不能代替固件的波特率切换命令。
    static const int bauds[] = { 115200, 4000000 };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    PushLog("[MAKCU] 开始扫描 COM 口（115200 / 4000000，最长 6 秒）...");

    for (const auto& p : ports) {
        for (int b : bauds) {
            if (std::chrono::steady_clock::now() >= deadline) {
                makcu_set_error("自动检测超时（6 秒），未发现 MAKCU 设备");
                return false;
            }
            if (!makcu_open_port(p.c_str(), b))
                continue;
            if (makcu_handshake()) {
                { std::lock_guard<std::mutex> errorLock(g_makcu_errorMtx); g_makcu_lastError.clear(); }
                g_makcu_connected.store(true);
                g_makcu_moveLogged.store(0);
                makcu_rx_start();
                if (outPort) snprintf(outPort, 32, "%s", p.c_str());
                if (outBaud) *outBaud = b;
                char buf[160];
                snprintf(buf, sizeof(buf), "自动检测到盒子 (%s, %d baud) 官方ASCII固件", p.c_str(), b);
                PushLog(std::string("[MAKCU] ") + buf);
                return true;
            }
            makcu_close();
        }
    }

    makcu_set_error("未检测到盒子（已扫描所有 COM 口）");
    return false;
}

void makcu_close() {
    std::lock_guard<std::recursive_mutex> lifecycleLock(g_makcu_lifecycleMtx);
    makcu_rx_stop();   // 先停读线程，再释放按键/关句柄
    std::lock_guard<std::recursive_mutex> lock(g_makcu_writeMtx);
    if (g_makcu_hCom != INVALID_HANDLE_VALUE) {
        makcu_release_all();
        // WriteFile has completed each release. Do not discard queued TX bytes.
        CloseHandle(g_makcu_hCom);
        g_makcu_hCom = INVALID_HANDLE_VALUE;
        char b[96];
        snprintf(b, sizeof(b), "[MAKCU] 设备已关闭（本次共读走设备回显 %llu 字节）",
                 g_makcu_rxTotal.load(std::memory_order_relaxed));
        PushLog(b);
    } else {
        PushLog("[MAKCU] 设备已关闭");
    }
    g_makcu_connected = false;
}

bool makcu_is_connected() {
    return g_makcu_connected.load();
}

static const char* makcu_btn_cmd(unsigned char button) {
    switch (button) {
        case MAKCU_BTN_LEFT:   return "left";
        case MAKCU_BTN_RIGHT:  return "right";
        case MAKCU_BTN_MIDDLE: return "middle";
        case MAKCU_BTN_SIDE1:  return "ms1";
        case MAKCU_BTN_SIDE2:  return "ms2";
        default:               return nullptr;
    }
}

bool makcu_mouse_move(int dx, int dy) {
    int x = (std::clamp)(dx, -32768, 32767);
    int y = (std::clamp)(dy, -32768, 32767);
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "km.move(%d,%d)\r\n", x, y);
    if (!makcu_write_raw(cmd)) return false;
    if (g_makcu_moveLogged.fetch_add(1) < 3)
        PushLog(std::string("[MAKCU] 已完整发送（非 USB 执行确认）: ") + cmd);
    return true;
}

void makcu_mouse_button(unsigned char button, int state) {
    const char* name = makcu_btn_cmd(button);
    if (!name) return;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "km.%s(%d)\r\n", name, state ? 1 : 0);
    makcu_write_raw(cmd);
}

void makcu_mouse_left(int state)   { makcu_mouse_button(MAKCU_BTN_LEFT, state); }
void makcu_mouse_right(int state)  { makcu_mouse_button(MAKCU_BTN_RIGHT, state); }
void makcu_mouse_middle(int state) { makcu_mouse_button(MAKCU_BTN_MIDDLE, state); }

void makcu_mouse_wheel(char wheel) {
    int w = wheel < -127 ? -127 : (wheel > 127 ? 127 : wheel);
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "km.wheel(%d)\r\n", w);
    makcu_write_raw(cmd);
}

void makcu_mouse_all(unsigned char buttons, short dx, short dy, char wheel) {
    makcu_mouse_move(dx, dy);
    makcu_mouse_wheel(wheel);
    for (int i = 0; i < 5; i++) {
        unsigned char bit = (unsigned char)(1u << i);
        makcu_mouse_button(bit, (buttons & bit) ? 1 : 0);
    }
}

void makcu_release_all() {
    makcu_write_raw("km.left(0)\r\n");
    makcu_write_raw("km.right(0)\r\n");
    makcu_write_raw("km.middle(0)\r\n");
    makcu_write_raw("km.ms1(0)\r\n");
    makcu_write_raw("km.ms2(0)\r\n");
}

const char* makcu_get_last_error() {
    thread_local std::string error;
    std::lock_guard<std::mutex> lock(g_makcu_errorMtx);
    error = g_makcu_lastError;
    return error.c_str();
}

} // namespace makcu_provider::legacy
namespace makcu_provider::scheme4 {
namespace {
class Win32Ascii final : public Provider {
    std::string port_;
    int baud_ = 0;
public:
    ~Win32Ascii() override { close(); }
    bool connect(const std::string& port, int baud) override {
        port_ = port;
        baud_ = 0;
        if (port.empty()) {
            char found[32]{};
            int actual = 0;
            const bool ok = legacy::makcu_autodetect(found, &actual);
            if (ok) { port_ = found; baud_ = actual; }
            return ok;
        }
        // Explicit COM never falls back to another port or baud.
        const bool ok = legacy::makcu_init(port.c_str(), baud);
        if (ok) baud_ = baud;
        return ok;
    }
    void close() override { legacy::makcu_close(); }
    Status status() override {
        return {legacy::makcu_is_connected(), port_, baud_, baud_ != 0,
                legacy::makcu_get_last_error()};
    }
    bool move(int dx, int dy) override { return legacy::makcu_mouse_move(dx, dy); }
    void button(unsigned char b, bool down) override { legacy::makcu_mouse_button(b, down); }
    void wheel(int delta) override { legacy::makcu_mouse_wheel(static_cast<char>(delta)); }
    void releaseAll() override { legacy::makcu_release_all(); }
};
}
std::unique_ptr<Provider> create() { return std::make_unique<Win32Ascii>(); }
}
#endif
