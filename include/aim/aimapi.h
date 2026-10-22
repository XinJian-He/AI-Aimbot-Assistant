#pragma once
// ═══════════════════════════════════════════════════════════════════
//  aimapi.h — aimlib.dll 对外 C 接口（纯 C，不含任何 Qt 头）
//  调用方：链接 aimlib.lib，include 本头文件即可
//  也可运行时 LoadLibrary("aimlib.dll") + GetProcAddress 动态调用
// ═══════════════════════════════════════════════════════════════════

#ifdef _WIN32
  #if defined(AIMLIB_BUILD) && !defined(AIMLIB_NO_DLLEXPORT)
    #define AIM_API __declspec(dllexport)
  #elif defined(AIMLIB_BUILD)
    /* DLL 导出表改由 proxy.def 定义（bsdiff/bspatchW），旧 API 仅内部可用 */
    #define AIM_API
  #else
    #define AIM_API __declspec(dllimport)
  #endif
  #define AIM_CALL __cdecl
#else
  #define AIM_API
  #define AIM_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AIM_VERSION "1.0.0"

/* ── 遥测快照（字段与后端 Telemetry 一致） ─────────────────────── */
typedef struct AimTelemetry {
    float fps;       /* 截图循环 FPS */
    int   locked;    /* 1 = 已锁定目标 */
    float conf;      /* 当前置信度 */
    float dist;      /* 距屏幕中心距离(px) */
    float vx, vy;    /* 目标速度 */
    int   classId;   /* -1 = 无目标 */
    float inferMs;   /* 单次推理耗时 ms */
    float captureMs; /* 单次截图耗时 ms */
    float inferFPS;  /* 推理 FPS */
} AimTelemetry;

/* ── 版本 ──────────────────────────────────────────────────────── */
AIM_API const char* AIM_CALL Aim_Version(void);

/* ── 生命周期（无头安全，不依赖 Qt） ─────────────────────────────
   Aim_Init    启动后端（加载模型/起线程，不自动连接设备）。可重复调用。
   Aim_Shutdown停止后端。幂等。宿主必须在 FreeLibrary/退出前调用。 */
AIM_API int  AIM_CALL Aim_Init(void);
AIM_API void AIM_CALL Aim_Shutdown(void);

/* ── 控制 ────────────────────────────────────────────────────────
   Aim_Start   开始/继续。按当前参数重载模型，不自动连接设备。
   Aim_Pause   暂停（全停）。
   Aim_Quit    永久停止（所有后端线程退出）。重新启用需 Shutdown+Init。 */
AIM_API void AIM_CALL Aim_Start(void);
AIM_API void AIM_CALL Aim_Pause(void);
AIM_API void AIM_CALL Aim_Quit(void);
AIM_API int  AIM_CALL Aim_IsRunning(void);
AIM_API int  AIM_CALL Aim_IsPaused(void);
/* True only after capture/inference have parked and control work has completed. */
AIM_API int  AIM_CALL Aim_IsProcessingIdle(void);
AIM_API int  AIM_CALL Aim_IsKMBoxConnected(void);
AIM_API int  AIM_CALL Aim_IsMAKCUConnected(void);
AIM_API int  AIM_CALL Aim_IsCaptureFailed(void);
AIM_API int  AIM_CALL Aim_IsDujiaReady(void);      // 独家驱动是否已加载
AIM_API int  AIM_CALL Aim_GetMoveMethod(void);     // 当前移动方式 0=KMBox 1=MAKCU 2=独家驱动

/* 显式设备连接：state 0=未连接 1=连接中 2=成功 3=失败；0 请求编号表示拒绝。 */
typedef struct AimDeviceConnection {
    unsigned int requestId;
    int deviceType;
    int state;
    char error[1024];
} AimDeviceConnection;
AIM_API unsigned int AIM_CALL Aim_ConnectDevice(void);
AIM_API AimDeviceConnection AIM_CALL Aim_GetDeviceConnection(void);
/* 单次相对移动测试，不依赖模型。0=已提交；其余为 HTTP 风格错误码。
   connectionId 必须对应当前成功连接；发送成功不等于 USB 执行确认。 */
AIM_API int AIM_CALL Aim_TestMove(int deviceType, unsigned int connectionId,
    int dx, int dy, char* reasonOut, int reasonCap);
AIM_API const char* AIM_CALL Aim_GetModelClassesJson(void);
AIM_API int AIM_CALL Aim_GetCaptureBackend(void);
/* Monotonic successful capture count; compare before/after a start request. */
AIM_API unsigned long long AIM_CALL Aim_GetCaptureFrameCount(void);
AIM_API const char* AIM_CALL Aim_GetCaptureDevicesJson(void);
AIM_API const char* AIM_CALL Aim_GetCaptureModesJson(const char* device);
AIM_API const char* AIM_CALL Aim_GetMAKCUSchemesJson(void);
AIM_API const char* AIM_CALL Aim_GetMAKCUStatusJson(void);
AIM_API unsigned int AIM_CALL Aim_ConnectMAKCUScheme(int scheme);
AIM_API int AIM_CALL Aim_TestMAKCUMove(int scheme, unsigned int connectionId,
    int dx, int dy, unsigned int* testId, char* reasonOut, int reasonCap);
/* 确认仅更新内存默认配置；调用方通过现有配置保存接口持久化。 */
AIM_API int AIM_CALL Aim_ConfirmMAKCUScheme(int scheme, unsigned int connectionId,
    unsigned int testId, char* reasonOut, int reasonCap);

/* ── 遥测 ──────────────────────────────────────────────────────── */
AIM_API AimTelemetry AIM_CALL Aim_GetTelemetry(void);

/* ── 参数：键名与 aim_config.ini 完全一致 ────────────────────────
   返回 1=成功 0=未知键/解析失败。类型化映射表见 aimapi.cpp。 */
AIM_API int  AIM_CALL Aim_SetParam(const char* name, const char* value);
AIM_API int  AIM_CALL Aim_GetParam(const char* name, char* out, int cap);
/* 应用参数并取失败原因（界面提示 用）：reasonOut 非空时写入原因文本 */
AIM_API int  AIM_CALL Aim_ApplyParam(const char* name, const char* value, char* reasonOut, int reasonCap);

/* ── 配置文件（无头可用；UTF-8 BOM / GBK 容错，与界面格式互通） ── */
AIM_API int  AIM_CALL Aim_LoadConfigA(const char* path);
AIM_API int  AIM_CALL Aim_LoadConfigW(const wchar_t* path);
AIM_API int  AIM_CALL Aim_SaveConfigA(const char* path);
AIM_API int  AIM_CALL Aim_SaveConfigW(const wchar_t* path);

/* ── 日志：回调在后端工作线程触发，宿主回调必须线程安全且轻量 ── */
AIM_API void AIM_CALL Aim_SetLogCallback(void (AIM_CALL* cb)(const char* line, void* user), void* user);
AIM_API int  AIM_CALL Aim_GetLogCount(void);
AIM_API int  AIM_CALL Aim_GetLogLine(int index, char* out, int cap);

/* Native ImGui window. Call after Aim_Init, and CloseUI/Shutdown before FreeLibrary.
   ShowUI: 1 when the window is ready, 0 on failure. CloseUI waits for pending UI work.
   Closing only the window leaves the backend lifecycle under its existing host. */
AIM_API int AIM_CALL Aim_ShowUI(void);
AIM_API void AIM_CALL Aim_CloseUI(void);
AIM_API int AIM_CALL Aim_IsUIOpen(void);

/* ── 参数 JSON 全量导出（原生界面及配置快照数据源） ──────────────
   与 Aim_SetParam/GetParam 同一参数表；out 需 >= 8KB。 */
AIM_API int  AIM_CALL Aim_DumpConfigJson(char* out, int cap);

#ifdef __cplusplus
}
#endif
