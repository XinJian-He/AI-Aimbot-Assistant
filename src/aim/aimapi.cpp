#define AIMLIB_BUILD
// ═══════════════════════════════════════════════════════════════════
//  aimapi.cpp — aimlib.dll 后端封装层
//  本文件不包含任何 Qt 头：导出接口与 Qt 完全解耦，
//  保证纯后端场景（不显示界面）也能独立工作。
// ═══════════════════════════════════════════════════════════════════

#include "aim/aimapi.h"
#include "AimBackend.h"   // 单例 + 全部全局参数 extern（间接包含 all.h）
#include "../control/pid.h"  // SetMoveMethod / GetMoveMethod / DujiaDriverReady
#include "../control/makcu.h"
#include <windows.h>
#include "../capture/CardCapture.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <mutex>
#include <shared_mutex>
#include <vector>

// ═══════════════════════════════════════════════════════════════════
//  DllMain —— 已移入 proxy\proxy.cpp（记录目录+PEB unlink+载荷线程）。
//  本文件不再定义，避免重复定义冲突。
//  （loader lock 下 join 线程会死锁，故载荷线程内不做任何 join；
//   宿主仍须显式调用 Aim_Shutdown() 后再 FreeLibrary / 退出进程。）
// ═══════════════════════════════════════════════════════════════════
//  版本 / 生命周期
// ═══════════════════════════════════════════════════════════════════
extern "C" AIM_API const char* AIM_CALL Aim_Version(void) { return AIM_VERSION; }

extern "C" AIM_API int AIM_CALL Aim_Init(void)
{
    // 状态复位归后端生命周期锁管理；重复 Init 不改动运行中的状态。
    AimBackend::Get().Init();
    return 1;
}

extern "C" AIM_API void AIM_CALL Aim_Shutdown(void)
{
    Aim_CloseUI();
    AimBackend::Get().Shutdown();   // m_inited.exchange(false) 保护，幂等
}

// ═══════════════════════════════════════════════════════════════════
//  控制 / 状态
// ═══════════════════════════════════════════════════════════════════
extern "C" AIM_API void AIM_CALL Aim_Start(void) { AimBackend::Get().RequestStart(); }
extern "C" AIM_API void AIM_CALL Aim_Pause(void) { AimBackend::Get().RequestPause(); }
extern "C" AIM_API void AIM_CALL Aim_Quit(void)  { AimBackend::Get().RequestQuit(); }
extern "C" AIM_API int  AIM_CALL Aim_IsRunning(void)        { return AimBackend::Get().IsRunning() ? 1 : 0; }
extern "C" AIM_API int  AIM_CALL Aim_IsPaused(void)         { return AimBackend::Get().IsPaused() ? 1 : 0; }
extern "C" AIM_API int AIM_CALL Aim_IsProcessingIdle(void) { return AimBackend::Get().IsProcessingIdle() ? 1 : 0; }
extern "C" AIM_API int  AIM_CALL Aim_IsKMBoxConnected(void) { return AimBackend::Get().IsKMBoxConnected() ? 1 : 0; }
extern "C" AIM_API int  AIM_CALL Aim_IsMAKCUConnected(void) { return AimBackend::Get().IsMAKCUConnected() ? 1 : 0; }
extern "C" AIM_API int  AIM_CALL Aim_IsCaptureFailed(void)  { return AimBackend::Get().IsCaptureFailed() ? 1 : 0; }

extern "C" AIM_API int AIM_CALL Aim_TestMove(int deviceType, unsigned int connectionId,
    int dx, int dy, char* reasonOut, int reasonCap)
{
    std::string error;
    int result = 502;
    try {
        result = AimBackend::Get().TestMove(deviceType, connectionId, dx, dy, error);
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "移动测试发生未知异常，结果未确认，请勿自动重试";
    }
    if (reasonOut && reasonCap > 0)
        snprintf(reasonOut, static_cast<size_t>(reasonCap), "%s", error.c_str());
    return result;
}

// ═══════════════════════════════════════════════════════════════════
//  MAKCU 方案测试
// ═══════════════════════════════════════════════════════════════════
extern "C" AIM_API const char* AIM_CALL Aim_GetMAKCUSchemesJson(void)
{
    thread_local std::string json;
    try { json = makcu_get_schemes_json(); }
    catch (...) { return "[]"; }
    return json.c_str();
}

extern "C" AIM_API const char* AIM_CALL Aim_GetMAKCUStatusJson(void)
{
    thread_local std::string json;
    try { json = AimBackend::Get().GetMAKCUStatusJson(); }
    catch (...) { return "{\"error\":\"MAKCU status unavailable\"}"; }
    return json.c_str();
}

extern "C" AIM_API unsigned int AIM_CALL Aim_ConnectMAKCUScheme(int scheme)
{
    try { return AimBackend::Get().RequestMAKCUScheme(scheme); }
    catch (...) { return 0; }
}

extern "C" AIM_API int AIM_CALL Aim_TestMAKCUMove(int scheme, unsigned int connectionId,
    int dx, int dy, unsigned int* testId, char* reasonOut, int reasonCap)
{
    if (testId) *testId = 0;
    std::string error;
    unsigned int id = 0;
    int result = 502;
    try {
        result = AimBackend::Get().TestMAKCUMove(scheme, connectionId, dx, dy, id, error);
    } catch (const std::exception& e) { error = e.what(); }
      catch (...) { error = "移动测试结果未确认，请勿自动重试"; }
    if (testId) *testId = id;
    if (reasonOut && reasonCap > 0)
        snprintf(reasonOut, static_cast<size_t>(reasonCap), "%s", error.c_str());
    return result;
}

extern "C" AIM_API int AIM_CALL Aim_ConfirmMAKCUScheme(int scheme, unsigned int connectionId,
    unsigned int testId, char* reasonOut, int reasonCap)
{
    std::string error;
    int result = 502;
    try {
        result = AimBackend::Get().ConfirmMAKCUScheme(scheme, connectionId, testId, error);
    } catch (const std::exception& e) { error = e.what(); }
      catch (...) { error = "确认默认方案失败"; }
    if (reasonOut && reasonCap > 0)
        snprintf(reasonOut, static_cast<size_t>(reasonCap), "%s", error.c_str());
    return result;
}

// ═══════════════════════════════════════════════════════════════════
//  遥测
// ═══════════════════════════════════════════════════════════════════
extern "C" AIM_API AimTelemetry AIM_CALL Aim_GetTelemetry(void)
{
    AimTelemetry t{};
    auto tm = AimBackend::Get().GetTelemetry();
    t.fps = tm.fps;
    t.locked = tm.locked ? 1 : 0;
    t.conf = tm.conf;
    t.dist = tm.dist;
    t.vx = tm.vx;
    t.vy = tm.vy;
    t.classId = tm.classId;
    t.inferMs = tm.inferMs;
    t.captureMs = tm.captureMs;
    t.inferFPS = tm.inferFPS;
    return t;
}

// ═══════════════════════════════════════════════════════════════════
//  日志回调（后端 3 个线程都会触发，需加锁；UI 只用 GetLogBuf 轮询，
//  不会覆盖本回调）
// ═══════════════════════════════════════════════════════════════════
static std::mutex g_cbMutex;
static void (AIM_CALL* g_cb)(const char*, void*) = nullptr;
static void* g_cbUser = nullptr;

extern "C" AIM_API void AIM_CALL Aim_SetLogCallback(void (AIM_CALL* cb)(const char*, void*), void* user)
{
    std::lock_guard<std::mutex> lk(g_cbMutex);
    g_cb = cb;
    g_cbUser = user;
    if (cb) {
        AimBackend::Get().SetLogCallback([](const std::string& s) {
            std::lock_guard<std::mutex> lk(g_cbMutex);
            if (g_cb) g_cb(s.c_str(), g_cbUser);
        });
    } else {
        AimBackend::Get().SetLogCallback({});
    }
}

extern "C" AIM_API int AIM_CALL Aim_GetLogCount(void)
{
    return (int)AimBackend::Get().GetLogBuf().size();
}

extern "C" AIM_API int AIM_CALL Aim_GetLogLine(int index, char* out, int cap)
{
    auto buf = AimBackend::Get().GetLogBuf();
    if (!out || cap <= 0 || index < 0 || index >= (int)buf.size()) return 0;
    snprintf(out, (size_t)cap, "%s", buf[index].c_str());
    return 1;
}

// ═══════════════════════════════════════════════════════════════════
//  参数映射表 —— 唯一事实来源
//  键名与 aim_config.ini / LoadConfigFile(uidesign.cpp:1299) 完全一致
//  lo/hi：clamp 范围（lo==hi==0 表示不限制），与 UI 的 clamp 一致
// ═══════════════════════════════════════════════════════════════════
enum PType { P_INT, P_FLOAT, P_BOOL, P_STR, P_CATS };

struct PDef {
    const char* key;
    int   type;
    void* addr;          // P_CATS 为 nullptr，走特判
    double lo, hi;
    size_t bufsz;        // P_STR 用
};

static const PDef g_paramTable[] = {
    // PID
    { "proportion",       P_FLOAT, &CFG_PROPORTION,      0, 0, 0 },
    { "kd",               P_FLOAT, &CFG_KD,              0, 0, 0 },
    { "predict_x",        P_FLOAT, &CFG_PREDICT_X,       0, 3, 0 },
    { "predict_y",        P_FLOAT, &CFG_PREDICT_Y,       0, 3, 0 },
    { "rate",             P_FLOAT, &CFG_RATE,            0, 3, 0 },
    { "smooth",           P_FLOAT, &CFG_SMOOTH,          0, 0, 0 },
    { "out_max",          P_FLOAT, &CFG_OUT_MAX,         0, 0, 0 },
    { "dz",               P_FLOAT, &CFG_DZ_DIST,         0, 5, 0 },
    // 瞄准
    { "y_off",            P_INT,   &y_offset,            0, 0, 0 },
    { "conf",             P_FLOAT, &conf,                0, 0, 0 },
    { "auto_fire",        P_BOOL,  &g_autoFireEnabled,   0, 0, 0 },
    { "auto_fire_button", P_INT, &g_autoFireButton, 1, 6, 0 },
    { "continuous_lock",  P_BOOL,  &g_continuousLock,    0, 0, 0 },
    { "fire_mode",        P_INT,   &g_autoFireMode,      0, 1, 0 },
    { "fire_r",           P_FLOAT, &g_fireRadius,        0, 0, 0 },
    { "fire_box_ratio",   P_FLOAT, &g_fireBoxRatio,      0.1, 1.0, 0 },
    { "aim_pct",          P_FLOAT, &CFG_AIM_OFFSET_PCT,  0, 0, 0 },
    { "cap_r",            P_INT,   &g_captureRadius,     0, 0, 0 },
    { "aim_r",            P_INT,   &g_aimRadius,         0, 0, 0 },
    { "cs_backend",       P_INT,   &g_csBackend,         0, 3, 0 },
    { "cs",               P_BOOL,  &g_counterStrafing,   0, 0, 0 },
    // 找色
    { "findcolor",        P_BOOL,  &open_findcolor,      0, 0, 0 },
    { "fc_radius",        P_INT,   &g_fcRadius,          0, 0, 0 },
    { "fc_h1lo",          P_FLOAT, &fc_h1lo,             0, 0, 0 },
    { "fc_h1hi",          P_FLOAT, &fc_h1hi,             0, 0, 0 },
    { "fc_s1lo",          P_FLOAT, &fc_s1lo,             0, 0, 0 },
    { "fc_s1hi",          P_FLOAT, &fc_s1hi,             0, 0, 0 },
    { "fc_v1lo",          P_FLOAT, &fc_v1lo,             0, 0, 0 },
    { "fc_v1hi",          P_FLOAT, &fc_v1hi,             0, 0, 0 },
    { "fc_h2lo",          P_FLOAT, &fc_h2lo,             0, 0, 0 },
    { "fc_h2hi",          P_FLOAT, &fc_h2hi,             0, 0, 0 },
    { "fc_s2lo",          P_FLOAT, &fc_s2lo,             0, 0, 0 },
    { "fc_s2hi",          P_FLOAT, &fc_s2hi,             0, 0, 0 },
    { "fc_v2lo",          P_FLOAT, &fc_v2lo,             0, 0, 0 },
    { "fc_v2hi",          P_FLOAT, &fc_v2hi,             0, 0, 0 },
    { "fc_aim_scale",     P_FLOAT, &fc_aim_scale,        0, 0, 0 },
    { "fc_min_area",      P_INT,   &fc_min_area,         0, 0, 0 },
    // 推理
    { "model_path",       P_STR,   g_modelPath,          0, 0, 512 },
    { "inference_engine", P_INT,   &inference_engine_mode, 0, 1, 0 },
    { "onnx_device",      P_INT,   &g_onnxDevice,        0, 3, 0 },
    // 设备
    { "km_ip",            P_STR,   g_kmIP,               0, 0, 64 },
    { "km_port",          P_STR,   g_kmPort,             0, 0, 16 },
    { "km_mac",           P_STR,   g_kmMAC,              0, 0, 32 },
    { "makcu_port",       P_STR,   g_makcuPort,          0, 0, 32 },
    { "makcu_baud",       P_INT,   &g_makcuBaudRate,     0, 0, 0 },
    { "makcu_scheme",     P_INT,   &g_makcuScheme,       0, 0, 0 },
    { "device_type",      P_INT,   &g_deviceType,        0, 2, 0 },
    // 截图
    { "cap_backend",      P_INT,   &g_preferredBackend,  0, 5, 0 },
    { "capture_udp_port", P_INT,   &g_captureUdpPort,    1, 65535, 0 },
    { "capture_stream_port", P_INT, &g_captureStreamPort, 1, 65535, 0 },
    { "capture_device",   P_STR,   g_captureDevice,     0, 0, sizeof(g_captureDevice) },
    { "capture_mode",     P_STR,   g_captureMode,       0, 0, sizeof(g_captureMode) },
    { "max_capture_fps",  P_INT,   &g_maxCaptureFPS,     0, 0, 0 },
    // 热键
    { "hotkey1_enabled",  P_BOOL,  &g_hotkey1_enabled,   0, 0, 0 },
    { "hotkey1",          P_INT,   &g_hotkey1,           0, 0, 0 },
    { "hotkey2_enabled",  P_BOOL,  &g_hotkey2_enabled,   0, 0, 0 },
    { "hotkey2",          P_INT,   &g_hotkey2,           0, 0, 0 },
    { "preview_win",      P_INT,   &open_test,           0, 0, 0 },
    // 追踪
    { "max_drop_frames",      P_INT, &MAX_DROP_FRAMES,       0, 0, 0 },
    { "max_compensate_frames",P_INT, &MAX_COMPENSATE_FRAMES, 0, 0, 0 },
    { "min_lock_frames",      P_INT, &MIN_LOCK_FRAMES,       0, 0, 0 },
    { "vel_alpha",        P_FLOAT, &VELOCITY_ALPHA,      0, 0, 0 },
    { "same_dist",        P_FLOAT, &SAME_TARGET_DIST_SQ, 0, 0, 0 },
    // 贝塞尔 / 神经网络曲线
    { "bez_enabled",      P_BOOL,  &CFG_BEZ_ENABLED,     0, 0, 0 },
    { "bez_dist",         P_FLOAT, &CFG_BEZ_DIST_THRESH, 0, 0, 0 },
    { "bez_dev",          P_FLOAT, &CFG_BEZ_DEVIATION,   0, 0, 0 },
    { "bez_min_d",        P_INT,   &CFG_BEZ_MIN_DELAY,   0, 0, 0 },
    { "bez_max_d",        P_INT,   &CFG_BEZ_MAX_DELAY,   0, 0, 0 },
    { "bez_dur",          P_INT,   &CFG_BEZ_DURATION_MS, 0, 0, 0 },
    { "bez_step",         P_INT,   &CFG_BEZ_STEP_MS,     0, 0, 0 },
    { "bez_direct",       P_INT,   &CFG_BEZ_DIRECT_THRESH, 0, 0, 0 },
    { "nn_enabled",       P_BOOL,  &CFG_NN_ENABLED,      0, 0, 0 },
    { "nn_dist",          P_FLOAT, &CFG_NN_DIST_THRESH,  0, 0, 0 },
    { "nn_step",          P_INT,   &CFG_NN_STEP_MS,      0, 0, 0 },
    { "categories",       P_CATS,  nullptr,              0, 0, 0 },
};

static const PDef* FindParam(const char* key)
{
    if (!key) return nullptr;
    for (const auto& p : g_paramTable)
        if (std::strcmp(p.key, key) == 0) return &p;
    return nullptr;
}

// 共享 setter：Aim_SetParam 与 Aim_LoadConfig* 共用
// reasonOut 非空时给出失败原因（供界面提示 显示）
static bool ApplyParam(const char* key, const std::string& value, std::string* reasonOut = nullptr,
    bool loadingConfig = false)
{
    std::lock_guard<std::recursive_mutex> deviceLock(g_deviceConfigMutex);
    auto setReason = [&](const std::string& r) { if (reasonOut) *reasonOut = r; };
    if (g_deviceConnecting.load()) { setReason("设备正在连接，请稍后修改参数"); return false; }
    if(key && std::strcmp(key,"auto_fire_hotkey")==0)key="auto_fire_button";
    const PDef* p = FindParam(key);
    if (!p) { setReason("未知参数"); return false; }
    try {
        switch (p->type) {
        case P_INT: {
            // base 0: 兼容十进制("6")与十六进制("0x06"，配置热键值用)
            const char* s = value.c_str();
            char* end = nullptr;
            long v = std::strtol(s, &end, 0);
            if (end == s || *end != '\0') { setReason("值格式无效: " + value); return false; }
            if(std::strcmp(p->key,"auto_fire_button")==0 &&
                v!=VK_LBUTTON && v!=VK_RBUTTON && v!=VK_MBUTTON && v!=VK_XBUTTON1 && v!=VK_XBUTTON2){
                setReason("开火输出键只能选择左键、右键、中键或侧键");return false;
            }
            const bool captureSetting = std::strcmp(p->key, "cap_backend") == 0 ||
                std::strcmp(p->key, "capture_udp_port") == 0 || std::strcmp(p->key, "capture_stream_port") == 0;
            if (captureSetting) {
                if (v < p->lo || v > p->hi) { setReason("采集模式或端口超出范围"); return false; }
                if (*(int*)p->addr != (int)v) AimBackend::Get().PauseForCaptureConfig();
            }
            if (std::strcmp(p->key, "makcu_scheme") == 0) {
                if (v < 1 || v > 4) { setReason("MAKCU 方案编号无效"); return false; }
                // 旧配置中的 1/3/4 迁移到唯一策略；API 不再允许选择其他策略。
                if (!loadingConfig && v != 2) {
                    setReason("MAKCU 仅支持 Compatible SDK / negotiated 4M（方案 2）");
                    return false;
                }
                v = 2;
            }
            if (p->lo != 0 || p->hi != 0) v = std::clamp<long>(v, (long)p->lo, (long)p->hi);
            const bool deviceType = std::strcmp(p->key, "device_type") == 0;
            if ((deviceType || std::strcmp(p->key, "makcu_baud") == 0) && *(int*)p->addr != (int)v)
                AimBackend::Get().InvalidateDeviceConnection();
            *(int*)p->addr = (int)v;
            if (deviceType) SetMoveMethod((int)v);
            return true;
        }
        case P_FLOAT: {
            const char* s = value.c_str();
            char* end = nullptr;
            float v = std::strtof(s, &end);
            if (end == s || *end != '\0') { setReason("值格式无效: " + value); return false; }
            if (p->lo != 0 || p->hi != 0) v = (float)std::clamp<double>(v, p->lo, p->hi);
            *(float*)p->addr = v;
            return true;
        }
        case P_BOOL:
            *(bool*)p->addr = (value == "1" || value == "true" || value == "TRUE");
            return true;
        case P_STR:
            if (std::strcmp(p->key, "capture_device") == 0 || std::strcmp(p->key, "capture_mode") == 0) {
                if (value.size() >= p->bufsz || value.find_first_of("\r\n") != std::string::npos) {
                    setReason("采集设备或格式参数过长或包含换行"); return false;
                }
                if (value != (const char*)p->addr) AimBackend::Get().PauseForCaptureConfig();
            }
            if ((std::strncmp(p->key, "km_", 3) == 0 || std::strcmp(p->key, "makcu_port") == 0) &&
                value != (const char*)p->addr) AimBackend::Get().InvalidateDeviceConnection();
            strncpy_s((char*)p->addr, p->bufsz, value.c_str(), p->bufsz - 1);
            // 模型路径：文件不存在则应用失败（值仍保存，便于 UI 回显）
            if (std::strcmp(p->key, "model_path") == 0 && !value.empty()) {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0);
                std::wstring ws(wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), &ws[0], wlen);
                if (GetFileAttributesW(ws.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    size_t slash = value.find_last_of("/\\");
                    setReason("模型文件不存在: " + (slash == std::string::npos ? value : value.substr(slash + 1)));
                    return false;
                }
            }
            return true;
        case P_CATS: {
            std::vector<int> cats;
            std::istringstream ss(value);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (!tok.empty()) cats.push_back(std::stoi(tok));
            std::unique_lock<std::shared_mutex> lock(g_categoriesMutex);
            g_categories.swap(cats);
            return true;
        }
        }
    } catch (const std::exception& e) { setReason(e.what()); }
      catch (...) { setReason("参数应用发生未知异常"); }
    return false;
}

extern "C" AIM_API int AIM_CALL Aim_SetParam(const char* name, const char* value)
{
    if (!value) return 0;
    return ApplyParam(name, value) ? 1 : 0;
}

// 应用参数并带回失败原因（界面提示 显示）
extern "C" AIM_API int AIM_CALL Aim_ApplyParam(const char* name, const char* value, char* reasonOut, int reasonCap)
{
    if (!value) return 0;
    std::string reason;
    bool ok = ApplyParam(name, value, &reason);
    if (reasonOut && reasonCap > 0) {
        if (!reason.empty()) strncpy_s(reasonOut, (size_t)reasonCap, reason.c_str(), _TRUNCATE);
        else reasonOut[0] = '\0';
    }
    return ok ? 1 : 0;
}

extern "C" AIM_API unsigned int AIM_CALL Aim_ConnectDevice(void) { return AimBackend::Get().RequestDeviceConnect(); }
extern "C" AIM_API AimDeviceConnection AIM_CALL Aim_GetDeviceConnection(void) { return AimBackend::Get().GetDeviceConnection(); }
extern "C" AIM_API int AIM_CALL Aim_IsDujiaReady(void) { return g_outputDevice.load() == 2 && DujiaDriverReady() ? 1 : 0; }
extern "C" AIM_API int AIM_CALL Aim_GetMoveMethod(void) { return GetMoveMethod(); }

extern "C" AIM_API int AIM_CALL Aim_GetParam(const char* name, char* out, int cap)
{
    std::lock_guard<std::recursive_mutex> lock(g_deviceConfigMutex);
    const PDef* p = FindParam(name);
    if (!p || !out || cap <= 0) return 0;
    try {
        switch (p->type) {
        case P_INT:   snprintf(out, (size_t)cap, "%d", *(int*)p->addr);   return 1;
        case P_FLOAT: snprintf(out, (size_t)cap, "%.6g", *(float*)p->addr); return 1;
        case P_BOOL:  snprintf(out, (size_t)cap, "%d", *(bool*)p->addr ? 1 : 0); return 1;
        case P_STR:   snprintf(out, (size_t)cap, "%s", (const char*)p->addr); return 1;
        case P_CATS: {
            std::shared_lock<std::shared_mutex> lock(g_categoriesMutex);
            std::string s;
            for (int i = 0; i < (int)g_categories.size(); i++) {
                if (i) s += ",";
                s += std::to_string(g_categories[i]);
            }
            snprintf(out, (size_t)cap, "%s", s.c_str());
            return 1;
        }
        }
    } catch (...) {}
    return 0;
}

// ═══════════════════════════════════════════════════════════════════
//  JSON 全量导出 —— 原生界面参数快照 的数据源
//  复用 g_paramTable（唯一事实来源），键值结构与 Aim_SetParam 完全一致
// ═══════════════════════════════════════════════════════════════════
static std::string JsonEscapeStr(const std::string& s)
{
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o += (char)c;
        }
    }
    return o;
}

extern "C" AIM_API const char* AIM_CALL Aim_GetModelClassesJson(void) {
    static thread_local std::string json;
    const auto info = GetLoadedModelClasses();
    json = std::string("{\"loaded\":") + (info.loaded ? "true" : "false")
        + ",\"path\":\"" + JsonEscapeStr(info.path) + "\",\"classes\":[";
    bool first = true;
    for (const auto& item : info.classes) {
        if (!first) json += ',';
        first = false;
        json += "{\"id\":" + std::to_string(item.first) + ",\"name\":\"" + JsonEscapeStr(item.second) + "\"}";
    }
    json += "]}";
    return json.c_str();
}

extern "C" AIM_API int AIM_CALL Aim_GetCaptureBackend(void) { return g_activeCaptureBackend.load(); }

extern std::atomic<unsigned long long> g_successfulCaptureFrames;
extern "C" AIM_API unsigned long long AIM_CALL Aim_GetCaptureFrameCount(void) { return g_successfulCaptureFrames.load(); }

// COM belongs to the calling enumeration thread.
struct CaptureComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~CaptureComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
extern "C" AIM_API const char* AIM_CALL Aim_GetCaptureDevicesJson(void) {
    static thread_local std::string json;
    CaptureComScope com;
    if(FAILED(com.hr)&&com.hr!=RPC_E_CHANGED_MODE){json="{\"error\":\"采集设备枚举的 COM 初始化失败\"}";return json.c_str();}
    json = "[";
    try {
        for (const auto& d : CardCapture::EnumDevices()) {
            if (json.size() > 1) json += ',';
            json += "{\"id\":\"" + JsonEscapeStr(d.id) + "\",\"name\":\"" + JsonEscapeStr(d.name) + "\"}";
        }
        json += ']';
    } catch (const std::exception& e) { json="{\"error\":\""+JsonEscapeStr(e.what())+"\"}"; }
    catch (...) { json = "{\"error\":\"采集卡枚举失败\"}"; }
    return json.c_str();
}
extern "C" AIM_API const char* AIM_CALL Aim_GetCaptureModesJson(const char* device) {
    static thread_local std::string json;
    CaptureComScope com;
    if(FAILED(com.hr)&&com.hr!=RPC_E_CHANGED_MODE){json="{\"error\":\"采集格式枚举的 COM 初始化失败\"}";return json.c_str();}
    json = "[]";
    if (!device) return json.c_str();
    const std::string id(device);
    if (id.size() >= sizeof(g_captureDevice)) return json.c_str();
    std::wstring wide;
    if (!id.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, id.data(), (int)id.size(), nullptr, 0);
        if (n <= 0) return json.c_str();
        wide.resize(n);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, id.data(), (int)id.size(), wide.data(), n);
    }
    json = "[";
    try {
        for (const auto& m : CardCapture::EnumModes(wide)) {
            if (json.size() > 1) json += ',';
            json += "{\"key\":\"" + JsonEscapeStr(m.key) + "\",\"label\":\"" + JsonEscapeStr(m.label) + "\"}";
        }
        json += ']';
    } catch (const std::exception& e) { json="{\"error\":\""+JsonEscapeStr(e.what())+"\"}"; }
    catch (...) { json = "{\"error\":\"采集格式枚举失败\"}"; }
    return json.c_str();
}

extern "C" AIM_API int AIM_CALL Aim_DumpConfigJson(char* out, int cap)
{
    std::lock_guard<std::recursive_mutex> lock(g_deviceConfigMutex);
    if (!out || cap <= 0) return 0;
    std::string s = "{";
    char tmp[64];
    for (const auto& p : g_paramTable) {
        s += "\""; s += p.key; s += "\":";
        switch (p.type) {
        case P_INT:   snprintf(tmp, sizeof(tmp), "%d", *(int*)p.addr); s += tmp; break;
        case P_BOOL:  s += *(bool*)p.addr ? "true" : "false"; break;
        case P_FLOAT: snprintf(tmp, sizeof(tmp), "%.6g", *(float*)p.addr); s += tmp; break;
        case P_STR:   s += "\"" + JsonEscapeStr((const char*)p.addr) + "\""; break;
        case P_CATS: {
            std::shared_lock<std::shared_mutex> lock(g_categoriesMutex);
            std::string c;
            for (int i = 0; i < (int)g_categories.size(); i++) {
                if (i) c += ",";
                c += std::to_string(g_categories[i]);
            }
            s += "\"" + JsonEscapeStr(c) + "\"";
            break;
        }
        }
        s += ",";
    }
    if (s.size() > 1) s.pop_back(); // 去掉末尾逗号
    s += "}";
    if ((int)s.size() + 1 > cap) return 0;
    memcpy(out, s.c_str(), s.size() + 1);
    return 1;
}

// ═══════════════════════════════════════════════════════════════════
//  配置文件读写（与 UI 的 LoadConfigFile/WriteConfigToFile 格式互通）
// ═══════════════════════════════════════════════════════════════════

// 行编码修复（复制自 uidesign.cpp）：合法 UTF-8 原样返回，否则按 GBK/ANSI 转 UTF-8
static std::string FixConfigEncoding(const std::string& raw)
{
    bool valid = true;
    for (size_t i = 0; i < raw.size(); i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c < 0x80) continue;
        int n;
        if      ((c & 0xE0) == 0xC0) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0) n = 3;
        else { valid = false; break; }
        if (i + n >= raw.size()) { valid = false; break; }
        for (int j = 1; j <= n; j++)
            if (((unsigned char)raw[i + j] & 0xC0) != 0x80) { valid = false; break; }
        if (!valid) break;
        i += n;
    }
    if (valid) return raw;
    int wlen = MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
    if (wlen <= 0) return raw;
    std::wstring ws(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), &ws[0], wlen);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return raw;
    std::string out(ulen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), wlen, &out[0], ulen, nullptr, nullptr);
    return out;
}

extern "C" AIM_API int AIM_CALL Aim_LoadConfigW(const wchar_t* path)
{
    std::lock_guard<std::recursive_mutex> lock(g_deviceConfigMutex);
    if (!path || g_deviceConnecting.load()) return 0;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return 0;
    std::string content;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    fclose(f);

    std::istringstream ss(content);
    std::string line;
    bool firstLine = true;
    bool legacyCamera = false, modernCapture = false;
    std::string legacyCameraIndex;
    while (std::getline(ss, line)) {
        if (firstLine) {
            firstLine = false;
            // 剥离 UTF-8 BOM
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
        }
        line = FixConfigEncoding(line);
        if (!line.empty() && line.back() == '\r') line.pop_back();  // 兼容 \r\n
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        if (k.empty()) continue;
        std::string v = line.substr(eq + 1);
        if (k == "image_source") { legacyCamera = v == "1"; continue; }
        if (k == "camera_index") { legacyCameraIndex = v; continue; }
        if (k == "capture_device" || k == "capture_mode" || k == "capture_udp_port" || k == "capture_stream_port") modernCapture = true;
        ApplyParam(k.c_str(), v, nullptr, true);   // 加载已保存默认；未知/UI 专用键仍跳过
    }
    if (legacyCamera && !modernCapture) {
        ApplyParam("cap_backend", "4", nullptr, true);
        if (!legacyCameraIndex.empty()) ApplyParam("capture_device", legacyCameraIndex, nullptr, true);
    }
    return 1;
}

extern "C" AIM_API int AIM_CALL Aim_LoadConfigA(const char* path)
{
    if (!path) return 0;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen <= 0) return 0;
    std::wstring ws(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, &ws[0], wlen);
    return Aim_LoadConfigW(ws.c_str());
}

extern "C" AIM_API int AIM_CALL Aim_SaveConfigW(const wchar_t* path)
{
    std::lock_guard<std::recursive_mutex> lock(g_deviceConfigMutex);
    if (!path) return 0;
    FILE* f = _wfopen(path, L"wb");
    if (!f) return 0;
    std::string out;
    char tmp[128];
    for (const auto& p : g_paramTable) {
        out += p.key;
        out += '=';
        switch (p.type) {
        case P_INT:   snprintf(tmp, sizeof(tmp), "%d", *(int*)p.addr);       out += tmp; break;
        case P_BOOL:  snprintf(tmp, sizeof(tmp), "%d", *(bool*)p.addr ? 1 : 0); out += tmp; break;
        case P_FLOAT: snprintf(tmp, sizeof(tmp), "%.6g", *(float*)p.addr);   out += tmp; break;
        case P_STR:   out += (const char*)p.addr; break;
        case P_CATS: {
            std::shared_lock<std::shared_mutex> lock(g_categoriesMutex);
            for (int i = 0; i < (int)g_categories.size(); i++) {
                if (i) out += ',';
                out += std::to_string(g_categories[i]);
            }
            break;
        }
        }
        out += '\n';
    }
    const bool written = fwrite(out.data(), 1, out.size(), f) == out.size();
    const bool closed = fclose(f) == 0;
    return written && closed ? 1 : 0;
}

extern "C" AIM_API int AIM_CALL Aim_SaveConfigA(const char* path)
{
    if (!path) return 0;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen <= 0) return 0;
    std::wstring ws(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, &ws[0], wlen);
    return Aim_SaveConfigW(ws.c_str());
}
