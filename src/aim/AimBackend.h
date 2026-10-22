#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "all.h"
#include "aim/aimapi.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <deque>
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <cstdint>
#include "../detect/ModelClasses.h"

ModelClassInfo GetLoadedModelClasses();

// ═══════════════════════════════════════════════════════════════════
//  AimBackend — 自瞄后端单例，封装所有逻辑线程与全局参数
// ═══════════════════════════════════════════════════════════════════

class AimBackend {
public:
    static AimBackend& Get();

    // ── 生命周期 ──────────────────────────────────────────────
    void Init();
    void Shutdown();

    // ── 控制 ──────────────────────────────────────────────────
    unsigned int RequestDeviceConnect();
    AimDeviceConnection GetDeviceConnection();
    int TestMove(int deviceType, unsigned int connectionId, int dx, int dy, std::string& error);
    unsigned int RequestMAKCUScheme(int scheme);
    std::string GetMAKCUStatusJson();
    int TestMAKCUMove(int scheme, unsigned int connectionId, int dx, int dy,
        unsigned int& testId, std::string& error);
    int ConfirmMAKCUScheme(int scheme, unsigned int connectionId, unsigned int testId, std::string& error);
    void InvalidateDeviceConnection(); // 调用方持有 g_deviceConfigMutex
    void PauseForCaptureConfig(); // called under g_deviceConfigMutex
    void RequestStart();        // 开始/继续
    void RequestPause();        // 暂停（全停）
    void RequestQuit();         // 退出
    void RequestRegionApply();  // 应用截图区域变更

    bool IsRunning() const;
    bool IsPaused() const;
    bool IsProcessingIdle() const;
    bool IsKMBoxConnected() const;
    bool IsMAKCUConnected() const;
    bool IsCaptureFailed() const;

    // ── 日志回调（Qt 主线程安全读取） ──────────────────────────
    using LogCallback = std::function<void(const std::string&)>;
    void SetLogCallback(LogCallback cb);
    std::deque<std::string> GetLogBuf();

    // ── 遥测数据（线程安全快照） ──────────────────────────────
    struct Telemetry {
        float fps = 0;
        bool  locked = false;
        float conf = 0;
        float dist = 0;
        float vx = 0;
        float vy = 0;
        int   classId = -1;
        float inferMs = 0;
        float captureMs = 0;
        float inferFPS = 0;
    };
    Telemetry GetTelemetry();

    // 配置参数已在 AimBackend.cpp 中定义为全局变量，此处仅声明 extern 供外部引用

private:
    AimBackend() = default;
    ~AimBackend() { Shutdown(); }
    AimBackend(const AimBackend&) = delete;
    AimBackend& operator=(const AimBackend&) = delete;

    // ── 线程 ──────────────────────────────────────────────────
    std::thread m_captureThread;
    std::thread m_displayThread;
    std::thread m_mainCtrlThread;

    // ── 原子标志 ──────────────────────────────────────────────
    std::atomic<bool> m_quit{ false };
    std::atomic<bool> m_inited{ false };

    // ── 日志 ──────────────────────────────────────────────────
    std::mutex               m_logMutex;
    std::deque<std::string>  m_logBuf;
    static constexpr int     LOG_MAX = 300;
    LogCallback              m_logCallback;

    void PushLog(const std::string& s);
};

// ── 供 pid.cpp / findcolor.h 等后端代码引用的全局 PushLog ─────
void PushLog(const std::string& s);

// ── 遥测推送（由 DisplayLoop 调用） ───────────────────────────
void PushTelemetry(float fps, bool locked, float conf,
    float dist, float vx, float vy, int classId);

// ── 全局原子标志 ─────────────────────────────────────────────
extern std::recursive_mutex g_deviceConfigMutex;
extern std::atomic<bool> g_deviceConnecting;
extern std::atomic<bool> g_requestResume;
extern std::atomic<bool> g_requestPause;
extern std::atomic<bool> g_requestQuit;
extern std::atomic<bool> g_requestRegionApply;
extern std::atomic<bool> g_running;
extern std::atomic<bool> g_paused;
extern std::atomic<bool> g_kmConnected;
extern std::atomic<bool> g_makcuConnected;
extern std::atomic<float> g_captureMs;
extern std::atomic<float> g_inferMs;
extern std::atomic<float> g_inferFPS;

// ── 遥测原子 ─────────────────────────────────────────────────
extern std::atomic<float> g_telemetryFPS;
extern std::atomic<bool>  g_telemetryLocked;
extern std::atomic<float> g_telemetryConf;
extern std::atomic<float> g_telemetryDist;
extern std::atomic<float> g_telemetryVx;
extern std::atomic<float> g_telemetryVy;
extern std::atomic<int>   g_telemetryClassId;

// ── 自瞄参数 ─────────────────────────────────────────────────
extern int   y_offset;
extern int   MAX_DROP_FRAMES;
extern int   MAX_COMPENSATE_FRAMES;
extern int   MIN_LOCK_FRAMES;
extern float VELOCITY_ALPHA;
extern float conf;
extern float SAME_TARGET_DIST_SQ;
extern bool  g_autoFireEnabled;
extern int   g_autoFireButton;
extern bool  g_continuousLock;
extern int   g_autoFireMode;
extern float g_fireRadius;
extern float g_fireBoxRatio;
extern int   g_aimRadius;      // 自瞄半径：筛掉中心距屏幕中心超过此半径的识别框
extern float CFG_AIM_OFFSET_PCT;
extern bool  g_hotkey1_enabled;
extern bool  g_hotkey2_enabled;
extern int   g_hotkey1;
extern int   g_hotkey2;
extern bool  g_counterStrafing;
extern int   g_csBackend;

// ── 截图 ─────────────────────────────────────────────────────
extern int   g_captureRadius;
extern std::atomic<bool> g_captureFailed;
extern int   g_preferredBackend;   // 0=GDI 1=DXGI 2=WGC 3=UDP 4=Card 5=Stream
extern int   g_captureUdpPort;
extern int   g_captureStreamPort;
extern char  g_captureDevice[2048];
extern char  g_captureMode[128];
extern std::atomic<int> g_activeCaptureBackend;
extern int g_maxCaptureFPS;   // 截图帧率上限, 0=不限制 (默认180)

// ── 推理 ─────────────────────────────────────────────────────
extern int   inference_engine_mode;
extern int   g_onnxDevice;
extern int   g_modelInputSize;
extern bool  g_useManualModelSize;
extern bool  open_findcolor;
extern int   open_test;
extern char  g_modelPath[512];
extern char  g_kmIP[64];
extern char  g_kmPort[16];
extern char  g_kmMAC[32];
extern char  g_makcuPort[32];
extern int   g_makcuBaudRate;
extern int   g_makcuScheme;
extern int   g_deviceType;
extern std::vector<int> g_categories;
extern std::shared_mutex g_categoriesMutex;

// ── 屏幕 ─────────────────────────────────────────────────────
extern int   g_screenW;
extern int   g_screenH;
extern bool  g_useManualRes;

// ── 找色 ─────────────────────────────────────────────────────
extern float fc_h1lo, fc_h1hi, fc_s1lo, fc_s1hi, fc_v1lo, fc_v1hi;
extern float fc_h2lo, fc_h2hi, fc_s2lo, fc_s2hi, fc_v2lo, fc_v2hi;
extern int   fc_roi_x, fc_roi_y, fc_roi_w, fc_roi_h;
extern float fc_aim_scale;
extern int   fc_min_area;
extern int   g_fcRadius;

// ── PID/高级配置（pid.cpp 中定义，pid.h 已声明）───────────────
// 已通过 pid.h 包含，此处不重复声明
// CFG_PROPORTION, CFG_KD, CFG_PREDICT_X, CFG_PREDICT_Y, CFG_RATE, CFG_SMOOTH,
// CFG_OUT_MAX, CFG_DZ_DIST,
// CFG_BEZ_*, CFG_BEZ_ENABLED, g_moveMethod
