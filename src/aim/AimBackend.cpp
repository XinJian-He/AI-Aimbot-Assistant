#include "recent_log.h"
#include "AimBackend.h"
#include "pid.h"
#include "TargetPriority.h"
#include "../control/raw_input.h"
#include "KalmanFilter.h"   // KalmanP: Hungarian match + Kalman tracking
#include <mmsystem.h>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <shared_mutex>
#include <stdexcept>
#pragma comment(lib, "winmm.lib")

unsigned int xbox_mac = 0;

// ═══════════════════════════════════════════════════════════════════
//  全局原子标志（供所有后端代码引用）
// ═══════════════════════════════════════════════════════════════════
std::atomic<bool> g_requestResume{ false };
std::atomic<bool> g_requestPause{ false };
std::atomic<bool> g_requestQuit{ false };
std::atomic<bool> g_requestRegionApply{ false };
std::atomic<bool> g_running{ false };
std::atomic<bool> g_paused{ true };
std::atomic<bool> g_kmConnected{ false };
std::atomic<bool> g_makcuConnected{ false };
std::recursive_mutex g_deviceConfigMutex;
std::atomic<bool> g_deviceConnecting{ false };
static std::mutex g_backendLifecycleMutex;
static std::mutex g_connectionMutex;
static AimDeviceConnection g_connection{};
static std::atomic<bool> g_requestDeviceConnect{ false };
// 以下方案/测试状态由 g_deviceConfigMutex 保护；候选不覆盖持久化默认值。
static int g_requestedMAKCUScheme = 2;
static unsigned int g_nextMAKCUTestId = 0;
static struct {
    unsigned int testId = 0;
    unsigned int connectionId = 0;
    int scheme = 0;
    int dx = 0;
    int dy = 0;
    bool submitted = false;
} g_lastMAKCUTest;
std::atomic<float> g_captureMs{ 0.f };
std::atomic<float> g_inferMs{ 0.f };
std::atomic<float> g_inferFPS{ 0.f };

// 遥测
std::atomic<float> g_telemetryFPS{ 0 };
std::atomic<bool>  g_telemetryLocked{ false };
std::atomic<float> g_telemetryConf{ 0 };
std::atomic<float> g_telemetryDist{ 0 };
std::atomic<float> g_telemetryVx{ 0 };
std::atomic<float> g_telemetryVy{ 0 };
std::atomic<int>   g_telemetryClassId{ -1 };

// ═══════════════════════════════════════════════════════════════════
//  全局配置变量（后端代码直接引用，定义在此处）
// ═══════════════════════════════════════════════════════════════════

// 自瞄
int   y_offset = 12;
int   MAX_DROP_FRAMES = 3;
int   MAX_COMPENSATE_FRAMES = 0;
constexpr int COMPENSATE_MAX_OFFSET_PX = 5;   // 丢帧补偿框距上一帧真实框的最大偏移（每轴）
int   MIN_LOCK_FRAMES = 1;
float VELOCITY_ALPHA = 0.35f;
float conf = 0.25f;
float SAME_TARGET_DIST_SQ = 2500.0f;
bool  g_autoFireEnabled = false;
int   g_autoFireButton = VK_LBUTTON;
bool  g_continuousLock = false;   // 持续锁定：开启后无需按热键，始终锁定瞄准
int   g_autoFireMode = 1;   // 1=识别框比例
float g_fireRadius = 3.0f;
float g_fireBoxRatio = 0.8f;
float CFG_AIM_OFFSET_PCT = 0.0f;
bool  g_hotkey1_enabled = false;
bool  g_hotkey2_enabled = true;
int   g_hotkey1 = VK_LBUTTON;
int   g_hotkey2 = VK_RBUTTON;
bool  g_counterStrafing = false;
int   g_csBackend = 2;

// 截图
int   g_captureRadius = 160;
int   g_aimRadius = 160;   // 自瞄半径（与截图半径同默认：圆内才参与瞄准）
int   g_preferredBackend = 1;   // 0=GDI 1=DXGI 2=WGC 3=UDP 4=Card 5=Stream
int   g_captureUdpPort = 7788;
int   g_captureStreamPort = 9339;
char  g_captureDevice[2048] = "";
char  g_captureMode[128] = "";
std::atomic<int> g_activeCaptureBackend{-1};
std::atomic<unsigned long long> g_successfulCaptureFrames{0};

// 推理
int   inference_engine_mode = 1;
int   g_onnxDevice = 1;
int   g_modelInputSize = 224;
bool  g_useManualModelSize = false;
bool  open_findcolor = false;
int   open_test = 1;
// proxy dir recorded in DllMain before PEB unlink; GetModuleFileNameW
// cannot find this module after unlink, so prefer g_proxyDir first
// (same as web_server/integration).
extern "C" wchar_t g_proxyDir[MAX_PATH];
char  g_modelPath[512] = "";
char  g_kmIP[64] = "192.168.2.188";
char  g_kmPort[16] = "1000";
char  g_kmMAC[32] = "25ABDBB2";
char  g_makcuPort[32] = "COM3";
int   g_makcuBaudRate = 115200;
int   g_makcuScheme = 2;   // 固定使用 Compatible SDK / negotiated 4M
int   g_deviceType = 2;   // 2=独家驱动
std::vector<int> g_categories = { 0, 5 };
std::shared_mutex g_categoriesMutex;

// 屏幕
int   g_screenW = 0;
int   g_screenH = 0;
bool  g_useManualRes = false;

// 找色
float fc_h1lo = 0, fc_h1hi = 3, fc_s1lo = 100, fc_s1hi = 255, fc_v1lo = 120, fc_v1hi = 255;
float fc_h2lo = 177, fc_h2hi = 180, fc_s2lo = 100, fc_s2hi = 255, fc_v2lo = 100, fc_v2hi = 255;
int   g_fcRadius = 40;
float fc_aim_scale = 0.7f;
int   fc_min_area = 2;

// 找色 ROI（由 DisplayLoop 根据 g_fcRadius 推导）
int   fc_roi_x = -40, fc_roi_y = -40, fc_roi_w = 80, fc_roi_h = 80;

// ═══════════════════════════════════════════════════════════════════
//  后端状态
// ═══════════════════════════════════════════════════════════════════
static std::unique_ptr<Detector> g_detector;
static std::mutex g_modelClassesMutex;
static ModelClassInfo g_modelClasses;
ModelClassInfo GetLoadedModelClasses() {
    std::lock_guard<std::mutex> lock(g_modelClassesMutex);
    return g_modelClasses;
}
static void PublishModelClasses(const std::string& path, const Detector* detector = nullptr) {
    ModelClassInfo info;
    info.path = path;
    info.loaded = detector != nullptr;
    if (detector) info.classes = detector->GetClasses();
    std::lock_guard<std::mutex> lock(g_modelClassesMutex);
    g_modelClasses = std::move(info);
}
static ScreenCapture              g_capture;
static std::atomic<bool>          g_capParked{ false };
static std::atomic<bool>          g_dispParked{ false };

// ═══════════════════════════════════════════════════════════════════
//  OpenCV preview window (dedicated thread, inference loop never
//  blocks on imshow/waitKey). DisplayLoop pushes into a swap buffer
//  (newest wins, stale frames dropped); PreviewLoop draws each new
//  result once and uses waitKey(1) only to pump window messages.
//  Background-window paint stalls only affect this thread.
// ═══════════════════════════════════════════════════════════════════
static std::mutex    g_previewMutex;
static cv::Mat       g_previewFrame;         // 待显示帧（锁内深拷贝，与 display 不共享像素）
static bool          g_previewDirty = false; // 有待显示的帧（预览线程消费后清零）
static bool          g_previewWindowOpen = false;
static std::thread   g_previewThread;

struct ImageData {
    std::vector<uint8_t> data;
    int width;
    int height;
};
static std::shared_ptr<ImageData> g_latestFrame{ nullptr };
static std::mutex                 g_frameMutex;
static std::atomic<double>        g_captureFPS{ 0.0 };

// 截图帧率上限（0=不限制）。默认 180：DML 推理与 DXGI 截图共用同一块 GPU，
// 无节制跑满会互相抢占导致帧率飘忽；sleep_until 节拍让负载可控
// （同参考模块 frame.cpp 的 maxFPS 节流写法）
int g_maxCaptureFPS = 180;

static std::atomic<int> g_capW{ g_captureRadius * 2 };
static std::atomic<int> g_capH{ g_captureRadius * 2 };
static const char* g_backendNames[] = { "GDI", "DXGI", "WGC", "UDP", "采集卡", "串流" };

// 截图失败标志（供UI显示原因）
std::atomic<bool> g_captureFailed{false};

// ═══════════════════════════════════════════════════════════════════
//  PushLog / PushTelemetry
// ═══════════════════════════════════════════════════════════════════
static std::mutex               g_logMutex;
static RecentLog::Buffer        g_logBuf;
static AimBackend::LogCallback  g_logCallback;

void PushLog(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logBuf.Append(s);
    printf("%s\n", s.c_str());
    if (g_logCallback) g_logCallback(s);

}

void PushTelemetry(float fps, bool locked, float confVal,
    float dist, float vx, float vy, int classId)
{
    g_telemetryFPS.store(fps);
    g_telemetryLocked.store(locked);
    g_telemetryConf.store(confVal);
    g_telemetryDist.store(dist);
    g_telemetryVx.store(vx);
    g_telemetryVy.store(vy);
    g_telemetryClassId.store(classId);
    g_inferFPS.store(fps);
}

// ═══════════════════════════════════════════════════════════════════
//  AimBackend 单例
// ═══════════════════════════════════════════════════════════════════
AimBackend& AimBackend::Get() {
    static AimBackend inst;
    return inst;
}

void AimBackend::PushLog(const std::string& s) {
    ::PushLog(s);
}

void AimBackend::SetLogCallback(LogCallback cb) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logCallback = std::move(cb);
}

std::deque<RecentLog::Entry> GetRecentBackendLogs() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    return g_logBuf.Snapshot();
}
std::deque<std::string> AimBackend::GetLogBuf() {
    std::deque<std::string> result;
    for(const auto& entry:GetRecentBackendLogs())result.push_back(entry.text);
    return result;
}

AimBackend::Telemetry AimBackend::GetTelemetry() {
    Telemetry t;
    t.fps = g_telemetryFPS.load();
    t.locked = g_telemetryLocked.load();
    t.conf = g_telemetryConf.load();
    t.dist = g_telemetryDist.load();
    t.vx = g_telemetryVx.load();
    t.vy = g_telemetryVy.load();
    t.classId = g_telemetryClassId.load();
    t.inferMs = g_inferMs.load();
    t.captureMs = g_captureMs.load();
    t.inferFPS = g_inferFPS.load();
    return t;
}

bool AimBackend::IsRunning() const { return g_running.load(); }
bool AimBackend::IsPaused() const  { return g_paused.load(); }
bool AimBackend::IsProcessingIdle() const {
    std::unique_lock<std::recursive_mutex> lock(g_deviceConfigMutex,std::try_to_lock);
    return lock.owns_lock() && !g_running.load() && g_paused.load() &&
        g_capParked.load() && g_dispParked.load() && !g_deviceConnecting.load() &&
        !g_requestResume.load() && !g_requestPause.load() && !g_requestRegionApply.load();
}
bool AimBackend::IsKMBoxConnected() const  { return g_kmConnected.load(); }
bool AimBackend::IsMAKCUConnected() const { return g_makcuConnected.load() && makcu_is_connected(); }

static void StopAndWaitParked();

unsigned int AimBackend::RequestDeviceConnect() {
    std::lock_guard<std::recursive_mutex> configLock(g_deviceConfigMutex);
    if (!m_inited.load() || g_requestQuit.load() || g_deviceConnecting.load()) return 0;
    g_requestedMAKCUScheme = 2;
    g_lastMAKCUTest = {};
    std::lock_guard<std::mutex> lock(g_connectionMutex);
    ++g_connection.requestId;
    if (!g_connection.requestId) ++g_connection.requestId;
    g_connection.deviceType = g_deviceType;
    g_connection.state = 1;
    g_connection.error[0] = '\0';
    g_deviceConnecting.store(true);
    g_requestResume.store(false);
    g_requestDeviceConnect.store(true);
    return g_connection.requestId;
}

unsigned int AimBackend::RequestMAKCUScheme(int scheme) {
    std::lock_guard<std::recursive_mutex> configLock(g_deviceConfigMutex);
    if (g_deviceType != 1 || scheme != 2) return 0;
    const unsigned int requestId = RequestDeviceConnect();
    if (requestId) g_requestedMAKCUScheme = scheme;
    return requestId;
}

std::string AimBackend::GetMAKCUStatusJson() {
    std::lock_guard<std::recursive_mutex> configLock(g_deviceConfigMutex);
    const auto connection = GetDeviceConnection();
    const auto& test = g_lastMAKCUTest;
    return "{\"defaultScheme\":" + std::to_string(g_makcuScheme) +
        ",\"activeScheme\":" + std::to_string(makcu_get_scheme()) +
        ",\"connectionId\":" + std::to_string(connection.requestId) +
        ",\"info\":" + makcu_get_info_json() + ",\"lastTest\":{\"testId\":" +
        std::to_string(test.testId) + ",\"scheme\":" + std::to_string(test.scheme) +
        ",\"connectionId\":" + std::to_string(test.connectionId) +
        ",\"dx\":" + std::to_string(test.dx) + ",\"dy\":" + std::to_string(test.dy) +
        ",\"submitted\":" + (test.submitted ? "true" : "false") + "}}";
}

int AimBackend::TestMAKCUMove(int scheme, unsigned int connectionId, int dx, int dy,
    unsigned int& testId, std::string& error) {
    testId = 0;
    std::unique_lock<std::recursive_mutex> configLock(g_deviceConfigMutex, std::try_to_lock);
    if (!configLock.owns_lock()) { error = "设备事务忙碌，请等待当前操作结束"; return 409; }
    // 新测试（包括失败）使上一次确认凭据失效，不重复提交运动。
    g_lastMAKCUTest = {};
    if (scheme != 2) { error = "MAKCU 方案编号无效"; return 400; }
    if (scheme != makcu_get_scheme()) { error = "MAKCU 方案已变更，请重新连接"; return 409; }
    ++g_nextMAKCUTestId;
    if (!g_nextMAKCUTestId) ++g_nextMAKCUTestId;
    testId = g_nextMAKCUTestId;
    g_lastMAKCUTest = {testId, connectionId, scheme, dx, dy, false};
    const int result = TestMove(1, connectionId, dx, dy, error);
    g_lastMAKCUTest.submitted = result == 0;
    return result;
}

int AimBackend::ConfirmMAKCUScheme(int scheme, unsigned int connectionId,
    unsigned int testId, std::string& error) {
    std::lock_guard<std::recursive_mutex> configLock(g_deviceConfigMutex);
    error.clear();
    if (scheme != 2 || !connectionId || !testId) {
        error = "确认参数无效";
        return 400;
    }
    const auto connection = GetDeviceConnection();
    if (!m_inited.load() || g_requestQuit.load() || !makcu_is_connected()) {
        error = "MAKCU 未连接";
        return 503;
    }
    if (g_deviceConnecting.load() || g_deviceType != 1 || connection.deviceType != 1 ||
        connection.state != 2 || connection.requestId != connectionId ||
        scheme != makcu_get_scheme() || g_outputDevice.load() != 1 ||
        g_lastMAKCUTest.testId != testId || !g_lastMAKCUTest.submitted ||
        g_lastMAKCUTest.scheme != scheme || g_lastMAKCUTest.connectionId != connectionId) {
        error = "测试记录已失效，请在当前连接下重新测试并观察实际移动";
        return 409;
    }
    g_makcuScheme = scheme;
    return 0;
}

AimDeviceConnection AimBackend::GetDeviceConnection() {
    std::lock_guard<std::mutex> lock(g_connectionMutex);
    return g_connection;
}

int AimBackend::TestMove(int deviceType, unsigned int connectionId, int dx, int dy, std::string& error) {
    std::unique_lock<std::recursive_mutex> configLock(g_deviceConfigMutex, std::try_to_lock);
    error.clear();
    if (!configLock.owns_lock()) { error = "设备事务忙碌，请等待当前操作结束"; return 409; }
    if (deviceType < 0 || deviceType > 2 || !connectionId ||
        dx < -32768 || dx > 32767 || dy < -32768 || dy > 32767 || (!dx && !dy)) {
        error = "移动测试参数无效";
        return 400;
    }
    if (!m_inited.load() || g_requestQuit.load()) {
        error = "后端未就绪或正在退出";
        return 503;
    }
    if (g_deviceConnecting.load() || g_running.load() || !g_paused.load() ||
        g_requestResume.load() || g_requestPause.load() || g_requestRegionApply.load() ||
        !g_capParked.load() || !g_dispParked.load()) {
        error = "请等待设备连接完成并暂停处理链路后测试";
        return 409;
    }
    const auto connection = GetDeviceConnection();
    if (deviceType != g_deviceType || connection.deviceType != deviceType ||
        connectionId != connection.requestId) {
        error = "设备选择或连接已变更，请刷新状态后重试";
        return 409;
    }
    if (connection.state != 2 || g_outputDevice.load() != deviceType ||
        GetMoveMethod() != deviceType) {
        error = "所选设备尚未成功连接";
        return 503;
    }
    const int result = TestDeviceMove(deviceType, dx, dy, error);
    if ((result == 502 || result == 503) && deviceType == 1 && !makcu_is_connected()) {
        g_makcuConnected.store(false);
        g_outputDevice.store(-1);
        std::lock_guard<std::mutex> lock(g_connectionMutex);
        g_connection.state = 3;
        snprintf(g_connection.error, sizeof(g_connection.error), "%s", error.c_str());
    }
    if (result == 0) {
        PushLog("[移动测试] 已提交相对移动 (" + std::to_string(dx) + "," +
            std::to_string(dy) + ")，请观察设备输出");
    }
    return result;
}

void AimBackend::InvalidateDeviceConnection() {
    g_requestResume.store(false);
    g_lastMAKCUTest = {};
    if (m_inited.load() && (g_running.load() || !g_capParked.load() || !g_dispParked.load())) StopAndWaitParked();
    PauseMoveOutput();
    g_outputDevice.store(-1);
    g_kmConnected.store(false);
    kmNet_monitor_stop();
    g_makcuConnected.store(false);
    std::lock_guard<std::mutex> lock(g_connectionMutex);
    g_connection.state = 0;
    g_connection.error[0] = '\0';
}

// 仅显式连接调用。端口扫描由提供器在用户指定 AUTO 时执行，不跨方案回退。
bool EnsureMAKCUConnected(std::string* error) {
    const bool ok = makcu_select_scheme(g_requestedMAKCUScheme) &&
        InitMAKCUDriver(g_makcuPort, g_makcuBaudRate);
    g_makcuConnected.store(ok);
    if (!ok && error) *error = makcu_get_last_error();
    return ok;
}
bool AimBackend::IsCaptureFailed() const  { return g_captureFailed.load(); }

void AimBackend::PauseForCaptureConfig() {
    g_requestResume.store(false);
    if (m_inited.load() && (g_running.load() || !g_capParked.load() || !g_dispParked.load())) StopAndWaitParked();
}

void AimBackend::RequestStart() {
    std::lock_guard<std::recursive_mutex> lock(g_deviceConfigMutex);
    if (m_inited.load() && !g_requestQuit.load() && !g_deviceConnecting.load())
        g_requestResume.store(true);
}
void AimBackend::RequestPause() {
    std::lock_guard<std::recursive_mutex> lock(g_deviceConfigMutex);
    if (m_inited.load() && !g_requestQuit.load()) g_requestPause.store(true);
}
void AimBackend::RequestQuit() {
    std::lock_guard<std::recursive_mutex> lock(g_deviceConfigMutex);
    g_requestQuit.store(true);
}
void AimBackend::RequestRegionApply() {
    std::lock_guard<std::recursive_mutex> lock(g_deviceConfigMutex);
    if (m_inited.load() && !g_requestQuit.load()) g_requestRegionApply.store(true);
}

// ═══════════════════════════════════════════════════════════════════
//  辅助函数
// ═══════════════════════════════════════════════════════════════════

int GetEffectiveScreenW() {
    if (g_useManualRes && g_screenW > 0) return g_screenW;
    return GetSystemMetrics(SM_CXSCREEN);
}
int GetEffectiveScreenH() {
    if (g_useManualRes && g_screenH > 0) return g_screenH;
    return GetSystemMetrics(SM_CYSCREEN);
}

// ── VK 映射 ──────────────────────────────────────────────────
static int VkToHid(WORD vk) {
    switch (vk) {
    case 'A': return 0x04; case 'D': return 0x07;
    case 'W': return 0x1A; case 'S': return 0x16;
    default:  return 0;
    }
}
static WORD VkToScan(WORD vk) {
    switch (vk) {
    case 'A': return 0x1E; case 'D': return 0x20;
    case 'W': return 0x11; case 'S': return 0x1F;
    default:  return (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    }
}

// ── 急停注入 ─────────────────────────────────────────────────
static void CsKeyDown(WORD vk) {
    if (g_outputDevice.load() != GetMoveMethod()) return;
    switch (g_csBackend) {
    case 1: if (g_kmConnected) { int h = VkToHid(vk); if (h) kmNet_enc_keydown(h); } break;
    case 3: if (g_makcuConnected) { int h = VkToHid(vk); if (h) makcu_key_down((unsigned char)h); } break;
    case 2: {
        INPUT in = {}; in.type = INPUT_KEYBOARD;
        in.ki.wScan = VkToScan(vk); in.ki.dwFlags = KEYEVENTF_SCANCODE;
        SendInput(1, &in, sizeof(INPUT)); break;
    }
    default: break;
    }
}
static void CsKeyUp(WORD vk) {
    switch (g_csBackend) {
    case 1: if (g_kmConnected) { int h = VkToHid(vk); if (h) kmNet_enc_keyup(h); } break;
    case 3: if (g_makcuConnected) { int h = VkToHid(vk); if (h) makcu_key_up((unsigned char)h); } break;
    case 2: {
        INPUT in = {}; in.type = INPUT_KEYBOARD;
        in.ki.wScan = VkToScan(vk); in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT)); break;
    }
    default: break;
    }
}
void UIClearKeyMasks() {}

// KMBox 鼠标键只读盒子监听；静止按住不超时，断连/监听未开启不触发。
static bool IsMouseButton(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
        vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}
static bool ReadKMBoxMouse(int vk) {
    if (!g_kmConnected.load()) return false;
    switch (vk) {
    case VK_LBUTTON: return kmNet_monitor_mouse_left() == 1;
    case VK_RBUTTON: return kmNet_monitor_mouse_right() == 1;
    case VK_MBUTTON: return kmNet_monitor_mouse_middle() == 1;
    case VK_XBUTTON1: return kmNet_monitor_mouse_side1() == 1;
    case VK_XBUTTON2: return kmNet_monitor_mouse_side2() == 1;
    default: return false;
    }
}
static bool ReadControlKey(int vk) {
    if (g_moveMethod == MoveMethod::KMBOX && IsMouseButton(vk)) return ReadKMBoxMouse(vk);
    if (g_moveMethod == MoveMethod::MAKCU) return RawInput::IsPressed(vk);
    return RawInput::IsPressed(vk) || (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// 瞄准热键与找色右键共用入口；KMBox 鼠标键不回退系统或 Raw Input。
bool ReadHotkeyState(int vk) {
    if (g_moveMethod == MoveMethod::KMBOX && IsMouseButton(vk)) return ReadKMBoxMouse(vk);
    if (g_moveMethod == MoveMethod::MAKCU) return RawInput::IsPressed(vk);
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// ═══════════════════════════════════════════════════════════════════
//  主控工具：确定性全停
// ═══════════════════════════════════════════════════════════════════
static void StopAndWaitParked() {
    PauseMoveOutput();
    g_running.store(false, std::memory_order_release);
    g_paused.store(true, std::memory_order_release);
    g_capParked.store(false, std::memory_order_release);
    g_dispParked.store(false, std::memory_order_release);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!g_capParked.load(std::memory_order_acquire) ||
           !g_dispParked.load(std::memory_order_acquire)) {
        if (g_requestQuit.load(std::memory_order_acquire)) return;
        if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("等待采集/推理停止超时，请检查设备驱动或占用；本次操作未继续");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    PauseMoveOutput();
}

// ═══════════════════════════════════════════════════════════════════
//  CaptureLoop — 截图线程
// ═══════════════════════════════════════════════════════════════════
static bool CaptureInitToCurrentSize() {
    int w = g_capW.load(std::memory_order_acquire);
    int h = g_capH.load(std::memory_order_acquire);
    if (w <= 0 || h <= 0 || g_preferredBackend < 0 || g_preferredBackend > 5) {
        PushLog("[采集] 尺寸或模式无效");
        return false;
    }
    const bool desktop = g_preferredBackend <= 2;
    const int sw = GetEffectiveScreenW(), sh = GetEffectiveScreenH();
    if (desktop) { w = std::min(w, sw); h = std::min(h, sh); }
    g_capW.store(w, std::memory_order_release);
    g_capH.store(h, std::memory_order_release);
    g_capture.Reset();
    g_activeCaptureBackend.store(-1);
    // Preserve desktop fallback; external sources must never capture the local desktop instead.
    std::vector<int> choices{g_preferredBackend};
    if (desktop) for (int b : {1, 2, 0}) if (b != g_preferredBackend) choices.push_back(b);
    for (int b : choices) {
        if (g_capture.Init(w, h, b, g_captureDevice, g_captureMode, g_captureUdpPort, g_captureStreamPort)) {
            if (b <= 2 && !g_capture.SetRegion(std::max(0, (sw-w)/2), std::max(0, (sh-h)/2), w, h)) {
                g_capture.Reset();
                continue;
            }
            g_activeCaptureBackend.store(b);
            { std::lock_guard<std::mutex> lk(g_frameMutex); g_latestFrame.reset(); }
            PushLog(std::string("[采集] ") + g_backendNames[b] + " 已初始化 " + std::to_string(w) + "x" + std::to_string(h));
            return true;
        }
        PushLog(std::string("[采集] ") + g_backendNames[b] + " 初始化失败: " + g_capture.GetLastError());
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════
//  PreviewLoop - dedicated thread owning the OpenCV preview window.
//  Low-latency display: waitKey(1) is the message pump only (blocks
//  <=1ms); draw once per new inferred frame, paced by capture.
//  Frames are ping-pong swapped under lock (no
//  per-frame deep copy on the consumer side); imshow is called
//  outside the lock, so paint stalls never touch inference.
// ═══════════════════════════════════════════════════════════════════
static void PreviewLoop() {
    cv::Mat show;                      // consumer buffer (swap-in target)
    while (!g_requestQuit.load()) {
        if (open_test != 0) {
            bool haveNew = false;
            {
                std::lock_guard<std::mutex> lk(g_previewMutex);
                if (g_previewDirty) {
                    std::swap(g_previewFrame, show);   // ping-pong: move swap, no copy
                    g_previewDirty = false;
                    haveNew = true;
                }
            }
            if (haveNew && !show.empty()) {
                if (!g_previewWindowOpen) {
                    cv::namedWindow("Screen Capture Demo", cv::WINDOW_AUTOSIZE);
                    g_previewWindowOpen = true;
                }
                cv::imshow("Screen Capture Demo", show);
            }
            // waitKey(1): message pump only (redraw/close/key), blocks <=1ms.
            // No 30FPS throttle anymore - display age drops to ~1 frame.
            cv::waitKey(1);
        } else {
            // Preview off: destroy window (created & destroyed on this thread)
            if (g_previewWindowOpen) {
                cv::destroyWindow("Screen Capture Demo");
                g_previewWindowOpen = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    // Exit cleanup
    if (g_previewWindowOpen) {
        cv::destroyWindow("Screen Capture Demo");
        g_previewWindowOpen = false;
    }
}

static void CaptureLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int frameCount = 0;
    auto lastTime = std::chrono::high_resolution_clock::now();
    auto nextFrameTime = lastTime;   // 帧率节流基准（sleep_until 节拍）
    bool inited = false;
    auto lastFrameAt=std::chrono::steady_clock::now();
    bool noFrameReported=false;

    while (!g_requestQuit.load()) {
        if (!g_running.load(std::memory_order_acquire)) {
            if (inited) {
                g_capture.Reset();
                inited = false;
                g_activeCaptureBackend.store(-1);
                { std::lock_guard<std::mutex> lk(g_frameMutex); g_latestFrame.reset(); }
            }
            g_capParked.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (g_capParked.exchange(false, std::memory_order_acq_rel)) {
            try { inited = CaptureInitToCurrentSize(); }
            catch (const std::exception& e) { PushLog(std::string("[采集] 初始化异常: ") + e.what()); inited = false; }
            catch (...) { PushLog("[采集] 初始化异常"); inited = false; }
            if (!inited) { g_capture.Reset(); g_activeCaptureBackend.store(-1); }
            g_captureFailed.store(!inited, std::memory_order_release);
            frameCount = 0;lastFrameAt=std::chrono::steady_clock::now();noFrameReported=false;
            lastTime = std::chrono::high_resolution_clock::now();
            if (!inited) {
                g_running.store(false); g_paused.store(true);
                g_capParked.store(true);
                g_captureFailed.store(true, std::memory_order_release);
                PushLog("[截图] 初始化失败，已暂停采集");
                continue;
            }
        }
        if (!inited) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }

        int capW = g_capW.load(), capH = g_capH.load();
        size_t bufferSize = (size_t)capW * capH * 3;
        auto frame = std::make_shared<ImageData>();
        frame->width = capW; frame->height = capH;
        frame->data.resize(bufferSize);

        auto t0 = std::chrono::high_resolution_clock::now();
        const bool captured = g_capture.Capture(frame->data.data(), bufferSize, 50);
        if (captured) { frame->width = g_capture.GetWidth(); frame->height = g_capture.GetHeight(); }
        auto t1 = std::chrono::high_resolution_clock::now();
        if (!captured) {
            if(!noFrameReported&&std::chrono::steady_clock::now()-lastFrameAt>std::chrono::seconds(5)){
                PushLog("[采集] 设备已打开但连续 5 秒未收到画面，请检查输入信号、格式和其他程序占用");noFrameReported=true;
            }
            g_captureFPS.store(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        lastFrameAt=std::chrono::steady_clock::now();noFrameReported=false;
        float capMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
        g_captureMs.store(capMs);
        { std::lock_guard<std::mutex> lk(g_frameMutex); g_latestFrame = std::move(frame); }
        g_successfulCaptureFrames.fetch_add(1,std::memory_order_release);
        frameCount++;
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(now - lastTime).count();
        if (elapsed >= 1.0) { g_captureFPS = frameCount / elapsed; frameCount = 0; lastTime = now; }

        // ── 帧率限制（节流）：DML 推理与 DXGI 截图共用 GPU，无节制跑满会
        //    互相抢占导致帧率飘忽；sleep_until 节拍让负载可控（参考 frame.cpp）──
        int fpsLimit = g_maxCaptureFPS;
        if (fpsLimit > 0) {
            auto frameInterval = std::chrono::microseconds(1'000'000 / fpsLimit);
            nextFrameTime += frameInterval;
            auto now2 = std::chrono::high_resolution_clock::now();
            if (nextFrameTime > now2) {
                std::this_thread::sleep_until(nextFrameTime);
            } else {
                // 落后于计划（暂停恢复/卡顿），重置时间基准防止追赶
                nextFrameTime = now2;
            }
        }
    }
    g_capture.Reset();
    g_activeCaptureBackend.store(-1);
    CoUninitialize();
}

// ═══════════════════════════════════════════════════════════════════
//  DisplayLoop — 推理 + 自瞄主循环
// ═══════════════════════════════════════════════════════════════════
static void DisplayLoop() {
    std::shared_ptr<ImageData> prevFrame;
    cv::Mat display;
    int screenW = GetEffectiveScreenW(), screenH = GetEffectiveScreenH();
    bool hasLockedTarget = false;
    float lockedCX = 0.f, lockedCY = 0.f;
    Detection lockedTarget;
    int lockedTrackId = -1;      // 锁定轨迹的 track_id（匈牙利匹配保证身份稳定）
    int lockLostCount = 0;       // 锁定 id 从轨迹输出消失后的连续丢失帧数
    // 追踪器: 匈牙利匹配 + 卡尔曼滤波。GENERATE=最小锁定帧(确认轨迹所需命中帧数),
    // TERMINATE=最大丢失帧(丢失后卡尔曼预测继续存活帧数)。UI 两个滑块直接驱动。
    static KalmanP s_tracker;
    s_tracker.init(MIN_LOCK_FRAMES, MAX_DROP_FRAMES);

    bool cs_holdD = false, cs_holdA = false, cs_holdS = false, cs_holdW = false;
    auto csReleaseAll = [&]() {
        if (cs_holdD) { CsKeyUp('D'); cs_holdD = false; }
        if (cs_holdA) { CsKeyUp('A'); cs_holdA = false; }
        if (cs_holdS) { CsKeyUp('S'); cs_holdS = false; }
        if (cs_holdW) { CsKeyUp('W'); cs_holdW = false; }
    };

    auto lastFpsTime = std::chrono::high_resolution_clock::now();
    int inferCount = 0;
    auto inferFpsTimer = std::chrono::high_resolution_clock::now();
    bool wasRunning = false;
    int prevCsBackend = g_csBackend;

    auto csKeyUpVia = [&](int backend, WORD vk) {
        if (backend == 1 && g_kmConnected) { int h = VkToHid(vk); if (h) kmNet_enc_keyup(h); }
        else if (backend == 3 && g_makcuConnected) { int h = VkToHid(vk); if (h) makcu_key_up((unsigned char)h); }
        else if (backend == 2) {
            INPUT in = {}; in.type = INPUT_KEYBOARD;
            in.ki.wScan = VkToScan(vk); in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof(INPUT));
        }
    };

    while (!g_requestQuit.load()) {
        // Home 键边沿检测：切换隐藏 aim（注入宿主=隐藏 UI+OpenCV 窗口，独立模式=隐藏自身进程）
        static bool s_homePrev = false;
        bool homeNow = g_moveMethod == MoveMethod::MAKCU ? RawInput::IsPressed(VK_HOME)
            : (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (homeNow && !s_homePrev) ToggleDujiaHideSelf();
        s_homePrev = homeNow;

        int curCsBackend = g_csBackend;
        if (curCsBackend != prevCsBackend) {
            if (cs_holdD) { csKeyUpVia(prevCsBackend, 'D'); cs_holdD = false; }
            if (cs_holdA) { csKeyUpVia(prevCsBackend, 'A'); cs_holdA = false; }
            if (cs_holdS) { csKeyUpVia(prevCsBackend, 'S'); cs_holdS = false; }
            if (cs_holdW) { csKeyUpVia(prevCsBackend, 'W'); cs_holdW = false; }
            prevCsBackend = curCsBackend;
        }
        if (!g_running.load(std::memory_order_acquire)) {
            if (wasRunning) {
                csReleaseAll();
                hasLockedTarget = false; lockedTrackId = -1; lockLostCount = 0;
                cs_holdD = cs_holdA = cs_holdS = cs_holdW = false;
                prevFrame.reset();
                s_tracker.reset();   // 重新开始时清空轨迹，避免旧坐标残留
                // Note: preview window is owned by PreviewLoop; do not destroy cross-thread
            }
            wasRunning = false;
            g_dispParked.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (!wasRunning) {
            g_dispParked.store(false, std::memory_order_release);
            prevFrame.reset();
        }
        wasRunning = true;

        std::shared_ptr<ImageData> frame;
        { std::lock_guard<std::mutex> lk(g_frameMutex); frame = g_latestFrame; }
        if (!frame || frame == prevFrame) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        prevFrame = frame;
        if (frame->data.empty()) continue;
        if ((size_t)frame->width * frame->height * 3 != frame->data.size() || frame->width <= 0 || frame->height <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
        }

        const int captureW = frame->width, captureH = frame->height;
        const int screenCenterX = captureW / 2, screenCenterY = captureH / 2;
        const int captureOffsetX = (screenW - captureW) / 2, captureOffsetY = (screenH - captureH) / 2;

        cv::Mat frameMat(frame->height, frame->width, CV_8UC3, const_cast<uint8_t*>(frame->data.data()));
        if (display.rows != frame->height || display.cols != frame->width)
            display.create(frame->height, frame->width, CV_8UC3);
        frameMat.copyTo(display);
        cv::circle(display, cv::Point(screenCenterX, screenCenterY), 4, cv::Scalar(255, 255, 255), -1);
        // 自瞄圆圈：标出 g_aimRadius 范围（半径超出截图区域时截断到画面内）
        if (g_aimRadius > 0) {
            int aimR = std::min(g_aimRadius, std::min(captureW, captureH) / 2);
            cv::circle(display, cv::Point(screenCenterX, screenCenterY), aimR,
                       cv::Scalar(255, 0, 255), 1);
        }

        if (g_detector) {
            auto ti0 = std::chrono::high_resolution_clock::now();
            std::vector<Detection> results = g_detector->Inference(frameMat);
            auto ti1 = std::chrono::high_resolution_clock::now();
            float inferMs = std::chrono::duration<float, std::milli>(ti1 - ti0).count();
            g_inferMs.store(inferMs);
            inferCount++;
            double iElapsed = std::chrono::duration<double>(ti1 - inferFpsTimer).count();
            if (iElapsed >= 1.0) { g_inferFPS.store((float)(inferCount / iElapsed)); inferCount = 0; inferFpsTimer = ti1; }

            // 找色
            fc_roi_x = -g_fcRadius; fc_roi_y = -g_fcRadius;
            fc_roi_w = g_fcRadius * 2; fc_roi_h = g_fcRadius * 2;
            if (open_findcolor) {
                cv::Rect roi(screenCenterX + fc_roi_x, screenCenterY + fc_roi_y, fc_roi_w, fc_roi_h);
                roi &= cv::Rect(0, 0, frameMat.cols, frameMat.rows);
                cv::Mat roiMat = frameMat(roi);
                FindColor(roiMat, (float)screenW, (float)screenH);
                cv::rectangle(display, roi, cv::Scalar(0, 0, 255), 1);
                cv::putText(display, "FC_ROI", cv::Point(roi.x, std::max(roi.y - 4, 10)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.35, cv::Scalar(0, 0, 255), 1);
            }

            // ── 目标搜索 + 追踪：YOLO 检测 → 匈牙利匹配 + 卡尔曼滤波 ──
            //    类别过滤后的检测送入 KalmanP，返回已确认（命中 ≥ MIN_LOCK_FRAMES）的
            //    稳定轨迹；轨迹丢失后卡尔曼预测继续输出最多 MAX_DROP_FRAMES 帧。
            //    先按类别顺序选择；同类别保持原轨迹，丢失超限后选择最近备选。
            std::vector<DetectionObject> dets;
            std::vector<int> categoryOrder;
            {
                std::shared_lock<std::shared_mutex> lock(g_categoriesMutex);
                categoryOrder = g_categories;
            }
            {
                for (const auto& det : results) {
                    if (det.box.width <= 0 || det.box.height <= 0) continue;
                    if (det.conf < conf || std::isnan(det.conf)) continue;
                    bool wanted = false;
                    for (int cat : categoryOrder) { if (det.classId == cat) { wanted = true; break; } }
                    if (!wanted) continue;
                    // 自瞄半径过滤：识别框中心距屏幕中心超过 g_aimRadius 的框直接剔除，
                    // 框外人物不进入追踪/瞄准（0=不过滤）
                    if (g_aimRadius > 0) {
                        float dCX = det.box.x + det.box.width * 0.5f - screenCenterX;
                        float dCY = det.box.y + det.box.height * 0.5f - screenCenterY;
                        if (dCX * dCX + dCY * dCY > g_aimRadius * g_aimRadius) continue;
                    }
                    DetectionObject o;
                    o.label = det.classId;
                    o.prob = det.conf;
                    o.bbox.x = (float)det.box.x;
                    o.bbox.y = (float)det.box.y;
                    o.bbox.width = (float)det.box.width;
                    o.bbox.height = (float)det.box.height;
                    o.track_id = -1;
                    dets.push_back(o);
                }
            }
            auto tracks = s_tracker.predict(dets, (float)captureW, (float)captureH);

            // Class order outranks lock retention and distance. A higher-priority
            // confirmed class preempts the old lock; within one class retain its ID.
            const int bestClassRank = target_priority::BestRank(categoryOrder, tracks);
            if (hasLockedTarget && !target_priority::KeepLock(categoryOrder, lockedTarget.classId, bestClassRank)) {
                csReleaseAll();
                hasLockedTarget = false;
                lockedTrackId = -1;
                lockLostCount = 0;
            }

            // ── 同类别锁定策略：连续丢失超过上限才换目标 ──
            //    锁定 id 存活 → 持续刷新锁定（即使有更近目标也不换）；
            //    锁定 id 从轨迹输出消失（卡尔曼预测期结束）→ 连续丢失计数，
            //    期间锁在最后已知位置；连续丢失 ≥ MAX_DROP_FRAMES 帧才换最近的新轨迹。
            bool lockedAlive = false;
            bool switchReady = false;      // 有可切换的备选轨迹
            Detection switchTarget;
            float switchCX = 0.f, switchCY = 0.f;
            int switchTrackId = -1;
            float switchDistSq = FLT_MAX;
            for (const auto& tr : tracks) {
                const int classRank = target_priority::Rank(categoryOrder, tr.label);
                if (classRank >= static_cast<int>(categoryOrder.size()) || classRank != bestClassRank) continue;
                float cx = tr.bbox.x + tr.bbox.width * 0.5f;
                float cy = tr.bbox.y + tr.bbox.height * 0.5f;
                // 当前优先类别中的锁定 id 仍存活 → 刷新锁定
                if (hasLockedTarget && tr.track_id == lockedTrackId) {
                    lockedTarget.box = cv::Rect2f(tr.bbox.x, tr.bbox.y,
                        tr.bbox.width, tr.bbox.height);
                    lockedTarget.conf = tr.prob;
                    lockedTarget.classId = tr.label;
                    lockedCX = cx; lockedCY = cy;
                    lockLostCount = 0;
                    lockedAlive = true;
                    break;
                }
                // 备选：离屏幕中心最近的确认轨迹（仅在换目标时使用）
                float dSq = (float)((cx - screenCenterX) * (cx - screenCenterX) + (cy - screenCenterY) * (cy - screenCenterY));
                if (dSq < switchDistSq) {
                    switchDistSq = dSq;
                    switchTarget.box = cv::Rect2f(tr.bbox.x, tr.bbox.y,
                        tr.bbox.width, tr.bbox.height);
                    switchTarget.conf = tr.prob;
                    switchTarget.classId = tr.label;
                    switchCX = cx; switchCY = cy; switchTrackId = tr.track_id;
                    switchReady = true;
                }
            }

            if (hasLockedTarget) {
                if (lockedAlive) {
                    // 锁定刷新完成（lockLostCount 已清零）
                } else {
                    // 锁定 id 已从轨迹输出消失：连续丢失计数，锁在最后已知位置
                    lockLostCount++;
                    if (lockLostCount >= MAX_DROP_FRAMES) {
                        // 连续丢失超限 → 换到最近的备选轨迹（无备选则解锁）
                        if (switchReady) {
                            lockedTarget = switchTarget;
                            lockedCX = switchCX; lockedCY = switchCY;
                            lockedTrackId = switchTrackId;
                            lockedAlive = true;
                            lockLostCount = 0;
                        } else {
                            csReleaseAll();
                            hasLockedTarget = false;
                            lockedTrackId = -1;
                            lockLostCount = 0;
                        }
                    }
                }
            } else if (switchReady) {
                // 未锁定：直接锁定最近的确认轨迹
                lockedTarget = switchTarget;
                lockedCX = switchCX; lockedCY = switchCY;
                lockedTrackId = switchTrackId;
                lockLostCount = 0;
                hasLockedTarget = true;
                lockedAlive = true;
            }

        draw_and_send:
            if (hasLockedTarget) {
                // Quantize once at the integer output boundary, after the aim offset.
                const float offset = (std::clamp(CFG_AIM_OFFSET_PCT, -100.f, 100.f) / 100.f) * (lockedTarget.box.height * 0.5f);
                int localCX = std::clamp(cvRound(lockedCX), 0, captureW - 1);
                int localCY = std::clamp(cvRound(lockedCY + offset), 0, captureH - 1);
                const auto& box = lockedTarget.box;
                const cv::Rect pixelBox(cv::Point(cvRound(box.x), cvRound(box.y)),
                    cv::Point(cvRound(box.x + box.width), cvRound(box.y + box.height)));
                int boxH = pixelBox.height;
                int boxW = pixelBox.width;
                int offsetPx = cvRound(offset);
                int absoluteX = localCX + captureOffsetX;
                int absoluteY = localCY + captureOffsetY;

                bool _trig1 = g_hotkey1_enabled && ReadHotkeyState(g_hotkey1);
                bool _trig2 = g_hotkey2_enabled && ReadHotkeyState(g_hotkey2);
                bool _doAim = g_continuousLock || _trig1 || _trig2;

                bool fireNow = false;
                {
                    if (g_autoFireEnabled && lockedAlive) {
                        if (g_autoFireMode == 0) {
                            float _dc = sqrtf((float)((localCX - screenCenterX) * (localCX - screenCenterX) + (localCY - screenCenterY) * (localCY - screenCenterY)));
                            fireNow = (_dc < g_fireRadius);
                        } else {
                            float ratio = std::clamp(g_fireBoxRatio, 0.1f, 1.0f);
                            int innerW = (int)(boxW * ratio), innerH = (int)(boxH * ratio);
                            int innerX = pixelBox.x + (boxW - innerW) / 2;
                            int innerY = pixelBox.y + (boxH - innerH) / 2 + offsetPx;
                            innerX = std::max(innerX, pixelBox.x);
                            innerY = std::max(innerY, pixelBox.y);
                            if (innerX + innerW > pixelBox.x + boxW) innerX = pixelBox.x + boxW - innerW;
                            if (innerY + innerH > pixelBox.y + boxH) innerY = pixelBox.y + boxH - innerH;
                            fireNow = (screenCenterX >= innerX && screenCenterX < innerX + innerW && screenCenterY >= innerY && screenCenterY < innerY + innerH);
                        }
                    }
                }
                if (_doAim) {
                    if (_trig1) absoluteY += y_offset;
                    SendData(absoluteX, absoluteY, fireNow, 0, boxW, g_autoFireButton);

                    if (g_counterStrafing && g_csBackend != 0) {
                        bool _kA = ReadControlKey('A') && !cs_holdA;
                        bool _kD = ReadControlKey('D') && !cs_holdD;
                        bool _kW = ReadControlKey('W') && !cs_holdW;
                        bool _kS = ReadControlKey('S') && !cs_holdS;
                        bool wD = _kA && !_kD;
                        if (wD && !cs_holdD) { CsKeyDown('D'); cs_holdD = true; }
                        if (!wD && cs_holdD) { CsKeyUp('D'); cs_holdD = false; }
                        bool wA = _kD && !_kA;
                        if (wA && !cs_holdA) { CsKeyDown('A'); cs_holdA = true; }
                        if (!wA && cs_holdA) { CsKeyUp('A'); cs_holdA = false; }
                        bool wS = _kW && !_kS;
                        if (wS && !cs_holdS) { CsKeyDown('S'); cs_holdS = true; }
                        if (!wS && cs_holdS) { CsKeyUp('S'); cs_holdS = false; }
                        bool wW = _kS && !_kW;
                        if (wW && !cs_holdW) { CsKeyDown('W'); cs_holdW = true; }
                        if (!wW && cs_holdW) { CsKeyUp('W'); cs_holdW = false; }
                    }
                } else { csReleaseAll(); SubmitAutoFire(fireNow,g_autoFireButton); }

                // 绘制（橙色 = 锁定 id 已丢失，处于保持等待换目标阶段）
                cv::Scalar boxColor = (lockLostCount > 0) ? cv::Scalar(0, 165, 255) : cv::Scalar(0, 255, 0);
                // Draw floating-point endpoints with 1/16 px precision; avoid
                // truncating origin and size independently on every frame.
                constexpr int boxShift = 4;
                constexpr float boxScale = 16.f;
                cv::rectangle(display,
                    cv::Point(cvRound(box.x * boxScale), cvRound(box.y * boxScale)),
                    cv::Point(cvRound((box.x + box.width) * boxScale), cvRound((box.y + box.height) * boxScale)),
                    boxColor, 2, cv::LINE_AA, boxShift);
                cv::circle(display, cv::Point(localCX, localCY), 5, cv::Scalar(0, 0, 255), -1);
                cv::line(display, cv::Point(screenCenterX, screenCenterY), cv::Point(localCX, localCY), cv::Scalar(255, 0, 0), 1);
                float dist = sqrtf((float)((localCX - screenCenterX) * (localCX - screenCenterX) + (localCY - screenCenterY) * (localCY - screenCenterY)));
                PushTelemetry(g_inferFPS.load(), true, lockedTarget.conf, dist, 0.f, 0.f, lockedTarget.classId);
            }
        }
        if (!hasLockedTarget) { SubmitAutoFire(false,g_autoFireButton); PushTelemetry(g_inferFPS.load(), false, 0, 0, 0, 0, -1); }

        // FPS显示（画在预览帧上，由预览线程负责 imshow）
        auto nowFps = std::chrono::high_resolution_clock::now();
        float frameMs = std::chrono::duration<float, std::milli>(nowFps - lastFpsTime).count();
        float fps = (frameMs > 0.f) ? (1000.f / frameMs) : 0.f;
        lastFpsTime = nowFps;
        cv::putText(display, "FPS:" + std::to_string((int)fps), cv::Point(10, 25),
            cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);

        // Push preview frame to the dedicated thread only - inference loop
        // never touches window APIs. Keep only the freshest completed frame.
        if (open_test) {
            std::lock_guard<std::mutex> lk(g_previewMutex);
            // Single latest-frame slot: replace an unpainted old frame.
            display.copyTo(g_previewFrame);
            g_previewDirty = true;
        }
    }
    csReleaseAll();
}

// ═══════════════════════════════════════════════════════════════════
//  主控循环线程
// ═══════════════════════════════════════════════════════════════════
// Model auto-fallback: when model_path is empty, scan the DLL dir models\n// for a model (legacy layout model\ as fallback; prefer wa_v8_256.onnx,
// same as integration.cpp injection mode). Returns UTF-8 full path or empty.
static std::string FindDefaultModelPath()
{
    std::wstring dir;
    if (g_proxyDir[0]) {
        dir = g_proxyDir;                  // PEB unlink: GetModuleFileNameW fails
    } else {
        wchar_t buf[MAX_PATH] = {};
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&FindDefaultModelPath, &m);
        DWORD n = m ? GetModuleFileNameW(m, buf, MAX_PATH)
                    : GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n <= 0 || n >= MAX_PATH) return "";
        dir.assign(buf, n);
        size_t p = dir.find_last_of(L"\\/");
        if (p == std::wstring::npos) return "";
        dir = dir.substr(0, p + 1);
    }

    const wchar_t* subdirs[] = { L"models\\", L"model\\" };
    for (const wchar_t* sd : subdirs) {
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW((dir + sd + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::wstring best;   // first model file; wa_v8_256.onnx preferred
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = fd.cFileName;
            size_t dot = name.find_last_of(L'.');
            std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
            for (auto& c : ext) c = (wchar_t)towlower(c);
            if (ext != L".onnx" && ext != L".engine" && ext != L".trt" && ext != L".pt") continue;
            if (name == L"wa_v8_256.onnx") { best = dir + sd + name; break; }
            if (best.empty()) best = dir + sd + name;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (!best.empty()) {
            int wlen = WideCharToMultiByte(CP_UTF8, 0, best.c_str(), (int)best.size(), nullptr, 0, nullptr, nullptr);
            if (wlen > 0) {
                std::string out(wlen, '\0');
                WideCharToMultiByte(CP_UTF8, 0, best.c_str(), (int)best.size(), &out[0], wlen, nullptr, nullptr);
                return out;
            }
        }
    }
    return "";
}

static void ConnectSelectedDevice() {
    std::lock_guard<std::recursive_mutex> configLock(g_deviceConfigMutex);
    g_lastMAKCUTest = {};
    StopAndWaitParked();
    if (g_requestQuit.load()) return;
    const int type = g_deviceType; // 配置写入在连接忙碌期间被拒绝
    g_outputDevice.store(-1);
    g_kmConnected.store(false);
    kmNet_monitor_stop();
    g_makcuConnected.store(false);
    ShutdownMAKCUDriver();
    ShutdownDujiaDriver();
    std::string error;
    bool ok = false;
    try {
        if (type == 0) {
            if (!g_kmIP[0] || !g_kmPort[0] || !g_kmMAC[0]) error = "请填写 KMBox IP、端口和 UUID / MAC";
            else {
                const int ret = kmNet_init(g_kmIP, g_kmPort, g_kmMAC);
                ok = ret == 0;
                if (ok) {
                    const int monitorResult=kmNet_monitor(1);
                    ok=monitorResult==0;g_kmConnected.store(ok);
                    if(!ok)error="KMBox 鼠标监听启动失败（返回码: "+std::to_string(monitorResult)+"）";
                }
                else {
                    const char* reason = "设备连接未完成";
                    switch (ret) {
                    case err_creat_socket: reason = "创建网络套接字失败"; break;
                    case err_net_version: reason = "网络套接字版本错误"; break;
                    case err_net_tx: reason = "网络发送失败"; break;
                    case err_net_rx_timeout: reason = "等待设备响应超时，请检查供电、IP、端口及网络"; break;
                    case err_net_cmd: reason = "设备响应命令不匹配"; break;
                    case err_net_pts: reason = "设备响应序号不匹配"; break;
                    case err_mutex_timeout: reason = "设备通信忙碌，获取互斥锁超时"; break;
                    }
                    error = std::string(reason) + "（返回码: " + std::to_string(ret) + "）";
                }
            }
        } else if (type == 1) {
            ok = EnsureMAKCUConnected(&error);
        } else if (type == 2) {
            ok = InitDujiaDriver(&error);
        } else error = "不支持的设备类型";
    } catch (const std::exception& e) { error = e.what(); }
      catch (...) { error = "设备初始化发生未知异常"; }
    if (ok) { SetMoveMethod(type); g_outputDevice.store(type); }
    else {
        g_kmConnected.store(false); g_makcuConnected.store(false);
        ShutdownMAKCUDriver(); ShutdownDujiaDriver();
        if (error.empty()) error = "设备初始化失败，未返回详细原因";
    }
    {
        std::lock_guard<std::mutex> lock(g_connectionMutex);
        g_connection.state = ok ? 2 : 3;
        snprintf(g_connection.error, sizeof(g_connection.error), "%s", error.c_str());
    }
    PushLog(ok ? "[设备] 连接成功；处理流程保持暂停" : "[设备] 连接失败: " + error);
    g_deviceConnecting.store(false);
}

static void MainCtrlLoop() {
    auto nextLogCleanup=RecentLog::Clock::now();
    while (!g_requestQuit.load()) {
        const auto now=RecentLog::Clock::now();
        if(now>=nextLogCleanup){
            std::lock_guard<std::mutex> lock(g_logMutex);g_logBuf.Expire(now);
            nextLogCleanup=now+std::chrono::seconds(1);
        }
      try {
        if (g_requestDeviceConnect.exchange(false)) {
            ConnectSelectedDevice();
            continue;
        }
        if (g_makcuConnected.load() && !makcu_is_connected()) {
            std::lock_guard<std::recursive_mutex> deviceLock(g_deviceConfigMutex);
            PauseMoveOutput();
            g_lastMAKCUTest = {};
            g_makcuConnected.store(false);
            int expected = 1;
            g_outputDevice.compare_exchange_strong(expected, -1);
            const std::string error = makcu_get_last_error();
            {
                std::lock_guard<std::mutex> lock(g_connectionMutex);
                if (g_connection.deviceType == 1 && g_connection.state == 2) {
                    g_connection.state = 3;
                    snprintf(g_connection.error, sizeof(g_connection.error), "%s", error.c_str());
                }
            }
            PushLog("[设备] MAKCU 通信中断，请在设备页重新连接: " + error);
        }
        if (g_requestPause.exchange(false)) {
            std::lock_guard<std::recursive_mutex> lock(g_deviceConfigMutex);
            StopAndWaitParked(); PushLog("[系统] 已暂停（全部停止）");
            continue;
        }
        if (g_requestRegionApply.exchange(false)) {
            std::lock_guard<std::recursive_mutex> lock(g_deviceConfigMutex);
            bool wasRunning = g_running.load();
            StopAndWaitParked();
            if (g_requestQuit.load()) break;
            g_captureFailed.store(false, std::memory_order_release);
            {
                int capW = g_captureRadius * 2;
                int capH = g_captureRadius * 2;
                int sw = GetEffectiveScreenW(), sh = GetEffectiveScreenH();
                if (g_preferredBackend <= 2 && capW > sw) capW = sw;
                if (g_preferredBackend <= 2 && capH > sh) capH = sh;
                g_capW.store(capW, std::memory_order_release);
                g_capH.store(capH, std::memory_order_release);
            }
            { std::lock_guard<std::mutex> lk(g_frameMutex); g_latestFrame.reset(); }
            char buf[64]; snprintf(buf, sizeof(buf), "[截图] 区域变更 %dx%d", g_capW.load(), g_capH.load()); PushLog(buf);
            if (wasRunning) { ResumeMoveOutput(); g_capParked.store(true); g_paused.store(false); g_running.store(true); PushLog("[系统] 区域已生效，继续运行"); }
            continue;
        }
        if (g_requestResume.exchange(false)) {
            std::lock_guard<std::recursive_mutex> lock(g_deviceConfigMutex);
            if (g_deviceConnecting.load()) continue;
            PushLog("[系统] 收到开始/继续，重载配置…");
            StopAndWaitParked();
            if (g_requestQuit.load()) break;
            if (std::string(g_modelPath).empty()) {
                std::string autoModel = FindDefaultModelPath();
                if (!autoModel.empty()) {
                    strncpy_s(g_modelPath, sizeof(g_modelPath), autoModel.c_str(), _TRUNCATE);
                    char b[256]; snprintf(b, sizeof(b), "[����] ģ��·��Ϊ�գ��Զ�ѡ��: %s", autoModel.c_str()); PushLog(b);
                } else {
                    PushLog("[����] ģ��·��Ϊ�գ��޷���ʼ"); g_paused.store(true); g_running.store(false); continue;
                }
            }
            PublishModelClasses(g_modelPath);
            bool engineOk = false;
            try {
                g_detector.reset();
                g_detector = std::make_unique<Detector>(std::string(g_modelPath));
                int detected = g_detector->GetDetectedInputSize();
                if (!g_useManualModelSize && detected > 0) { g_modelInputSize = detected; char b[128]; snprintf(b, sizeof(b), "[引擎] 自动检测分辨率: %dx%d", detected, detected); PushLog(b); }
                g_detector->SetInputSize(g_modelInputSize);
                g_detector->SetONNXDevice(g_onnxDevice);
                PublishModelClasses(g_modelPath, g_detector.get());
                engineOk = true;
                char b[128]; snprintf(b, sizeof(b), "[引擎] 重载成功 %dx%d%s", g_modelInputSize, g_modelInputSize, g_useManualModelSize ? " (手动)" : ""); PushLog(b);
            } catch (const std::exception& e) {
                char b[256]; snprintf(b, sizeof(b), "[引擎] 重载失败: %s", e.what()); PushLog(b); g_detector.reset();
            }
            if (!engineOk) { PushLog("[系统] 引擎未就绪，保持暂停"); g_paused.store(true); g_running.store(false); continue; }
            g_captureFailed.store(false, std::memory_order_release);
            {
                int capW = g_captureRadius * 2;
                int capH = g_captureRadius * 2;
                int sw = GetEffectiveScreenW(), sh = GetEffectiveScreenH();
                if (g_preferredBackend <= 2 && capW > sw) capW = sw;
                if (g_preferredBackend <= 2 && capH > sh) capH = sh;
                g_capW.store(capW, std::memory_order_release);
                g_capH.store(capH, std::memory_order_release);
            }
            { std::lock_guard<std::mutex> lk(g_frameMutex); g_latestFrame.reset(); }
            // 设备连接独立于模型启动；未显式连接时不发送设备输出。
            if (g_outputDevice.load() != GetMoveMethod()) PushLog("[设备] 尚未连接，请在设备页点击连接");
            ResumeMoveOutput();
            g_capParked.store(true); g_paused.store(false); g_running.store(true); PushLog("[系统] 运行中");
            continue;
        }
      }catch(const std::exception& e){
        g_running.store(false);g_paused.store(true);g_requestResume.store(false);
        PauseMoveOutput();
        if(g_deviceConnecting.exchange(false)){
            std::lock_guard<std::mutex> lock(g_connectionMutex);g_connection.state=3;
            snprintf(g_connection.error,sizeof(g_connection.error),"%s",e.what());
        }
        PushLog(std::string("[系统] 控制操作失败: ")+e.what());
      }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Init / Shutdown
// ═══════════════════════════════════════════════════════════════════
void AimBackend::Init() {
    std::lock_guard<std::mutex> lifecycleLock(g_backendLifecycleMutex);
    std::lock_guard<std::recursive_mutex> configLock(g_deviceConfigMutex);
    if (m_inited.exchange(true)) return;
    SetConsoleOutputCP(65001); SetConsoleCP(65001);

    g_lastMAKCUTest = {};
    g_requestResume.store(false);
    g_requestPause.store(false);
    g_requestRegionApply.store(false);
    g_running.store(false);
    g_paused.store(true);
    g_requestQuit.store(false);
    g_requestDeviceConnect.store(false);
    g_deviceConnecting.store(false);
    g_outputDevice.store(-1);
    g_kmConnected.store(false); g_makcuConnected.store(false);
    SetMoveMethod(g_deviceType);
    ResumeMoveOutput();
    PauseMoveOutput();
    {
        std::lock_guard<std::mutex> lock(g_connectionMutex);
        g_connection.state = 0;
        g_connection.error[0] = '\0';
        g_connection.deviceType = g_deviceType;
    }
    PushLog("[设备] 等待在设备页手动连接");
    std::string path(g_modelPath);
    PublishModelClasses(path);

    g_capW.store(g_captureRadius * 2, std::memory_order_release);
    g_capH.store(g_captureRadius * 2, std::memory_order_release);
    char buf[128]; snprintf(buf, sizeof(buf), "[系统] 截图%dx%d 模型输入%dx%d", g_capW.load(), g_capH.load(), g_modelInputSize, g_modelInputSize); PushLog(buf);

    if (path.empty()) PushLog("[引擎] 未设置模型路径，启动后请在界面填写并点击开始");
    else {
        try {
            g_detector = std::make_unique<Detector>(path);
            int detected = g_detector->GetDetectedInputSize();
            if (!g_useManualModelSize && detected > 0) { g_modelInputSize = detected; char b[128]; snprintf(b, sizeof(b), "[引擎] 自动检测模型分辨率: %dx%d", detected, detected); PushLog(b); }
            g_detector->SetInputSize(g_modelInputSize);
            g_detector->SetONNXDevice(g_onnxDevice);
            PublishModelClasses(path, g_detector.get());
            char b[128]; snprintf(b, sizeof(b), "[引擎] 加载成功 %dx%d%s", g_modelInputSize, g_modelInputSize, g_useManualModelSize ? " (手动)" : ""); PushLog(b);
        } catch (const std::exception& e) {
            char b[256]; snprintf(b, sizeof(b), "[引擎] 加载失败: %s（保持暂停）", e.what()); PushLog(b); g_detector.reset();
        }
    }

    timeBeginPeriod(1);
    if (!RawInput::Start())
        PushLog("[输入] Raw Input 初始化失败，MAKCU 按键不可用: " + RawInput::GetLastError());

    m_captureThread = std::thread(CaptureLoop);
    m_displayThread = std::thread(DisplayLoop);
    g_previewThread = std::thread(PreviewLoop);

    g_running.store(false); g_paused.store(true);
    g_capParked.store(true); g_dispParked.store(true);
    PushLog("[系统] 已暂停（启动默认）。请设置模型路径后点击开始");

    m_mainCtrlThread = std::thread(MainCtrlLoop);
}

void AimBackend::Shutdown() {
    std::lock_guard<std::mutex> lifecycleLock(g_backendLifecycleMutex);
    {
        std::lock_guard<std::recursive_mutex> configLock(g_deviceConfigMutex);
        if (!m_inited.exchange(false)) return;
        g_requestQuit.store(true);
        g_requestResume.store(false);
        g_lastMAKCUTest = {};
        g_running.store(false); g_paused.store(true);
        PauseMoveOutput();
    }
    // 主控可能正在等待配置锁，不能持有该锁 join。
    RawInput::Stop();
    kmNet_monitor_stop();
    if (m_mainCtrlThread.joinable()) m_mainCtrlThread.join();
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_displayThread.joinable()) m_displayThread.join();
    PublishModelClasses("");
    ShutdownMoveOutput();
    {
        std::lock_guard<std::recursive_mutex> configLock(g_deviceConfigMutex);
        g_outputDevice.store(-1);
        ShutdownMAKCUDriver(); ShutdownDujiaDriver();
        g_kmConnected.store(false); g_makcuConnected.store(false);
        std::lock_guard<std::mutex> connectionLock(g_connectionMutex);
        g_connection.state = 0;
        g_connection.error[0] = '\0';
    }
    g_deviceConnecting.store(false);
    if (g_previewThread.joinable()) g_previewThread.join();
    timeEndPeriod(1);
}
