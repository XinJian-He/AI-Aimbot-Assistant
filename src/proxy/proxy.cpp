// ============================================================
//  proxy.cpp — 代理载荷（并入 aimlib.dll，伪装 SetupShell.dll）
//  腾讯视频启动按名加载 SetupShell.dll 时，本 DLL 被宿主
//  正常 OS 加载运行（因此内部可稳定承载复杂库/截图/D3D）。
//   - 转发原 DLL(SetupShell_orig.dll) 的 4 个 COM 导出，保证宿主功能正常
//   - DllMain 里起 WorkerThread 运行载荷（aim-DML 后端，见 integration.cpp）
//   - PEB unlink 隐蔽本 DLL 与原 DLL（模块枚举不可见）
//  注: aimlib.dll 被 host_test 等以原名加载时按 API 手动控制
//      (Aim_Init/Aim_Start...)，不自动起载荷；仅当模块名为
//      SetupShell.dll（或 AIM_PAYLOAD=1）时自动启动。
// ============================================================
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include "stealth/stealth.h"
#include "common/build_seed.h"

extern bool StartAim();
extern void StopAim();


// 代理 DLL 所在目录（DllMain 里在 PEB unlink 之前用 GetModuleFileNameW 取得，
// 之后模块被摘除，GetModuleHandleExW(FROM_ADDRESS) 会失败导致路径退回 exe 目录）
// selectany: uidesign.cpp 里也有同样的 COMDAT 弱定义兜底（EXE 构建不含本文件），
// 两者合并为同一符号，DllMain 写入对所有引用生效。
extern "C" __declspec(selectany) wchar_t g_proxyDir[MAX_PATH] = L"";

// 运行模式: 0=注入目标宿主(如 QQLive) 1=普通独立模式
// 由 WorkerThread 启动前按宿主 exe 判定, 也可用环境变量 AIM_MODE 覆盖(0/1)
// selectany 弱定义: pid.cpp 有同名 COMDAT 兜底(ui-design.exe 等不含本文件的构建),
// 两个弱定义由链接器合并为同一符号(与 g_proxyDir 相同机制)
extern "C" __declspec(selectany) int g_aimRunMode = 0;

// ---- 原 DLL 转发 ----
typedef HRESULT(STDAPICALLTYPE* pDllCanUnloadNow)();
typedef HRESULT(STDAPICALLTYPE* pDllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT(STDAPICALLTYPE* pDllRegisterServer)();
typedef HRESULT(STDAPICALLTYPE* pDllUnregisterServer)();

static HMODULE g_hOrig = nullptr;
static pDllCanUnloadNow     _CanUnload = nullptr;
static pDllGetClassObject   _GetClass  = nullptr;
static pDllRegisterServer   _RegSvr    = nullptr;
static pDllUnregisterServer _UnregSvr  = nullptr;

static HMODULE g_hModule = nullptr;

// 单例互斥名：随机化, 拟态系统对象(ctfmon/输入法风格), 避免 "GZai/Inject" 特征被枚举
// 每次编译由 BUILD_SEED 派生末尾, 换编译即换名。运行动态生成避免字符串明文
static char g_mutexName[96];
static const char* MakeMutexName() {
    // 每次运行随机后缀: 多实例/多机器相同编译物也不全同
    unsigned r = (unsigned)(GetTickCount64() ^ (unsigned long long)GetCurrentProcessId());
    r = (r * 1103515245u + 12345u) & 0x7FFFFFFFu;
    wsprintfA(g_mutexName, "Local\\MsCtfArch_Ctx_%08X_%08X", BUILD_SEED & 0x7FFFFFFFu, r);
    return g_mutexName;
}
static bool g_isFirstInstance = false;

// 主实例互斥: 同编译物固定名(BUILD_SEED 派生, 无随机后缀)。
// QQLive 多进程架构下主/子进程都加载本 DLL, 若各自初始化会弹出
// 多个控制面板并重复推理(抢 GPU/鼠标); 首个获得互斥的进程成为
// 唯一 aim 实例, 其余进程保持 COM 转发但跳过载荷。
static char g_mainMutexName[64];
static const char* MakeMainMutexName() {
    wsprintfA(g_mainMutexName, "Local\\MsCtfArch_Ctx_%08X", BUILD_SEED & 0x7FFFFFFFu);
    return g_mainMutexName;
}
static HANDLE g_hMainMutex = nullptr;
static bool AcquireMainInstance() {
    g_hMainMutex = CreateMutexA(nullptr, TRUE, MakeMainMutexName());
    if (g_hMainMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_hMainMutex); g_hMainMutex = nullptr;
        return false;
    }
    return g_hMainMutex != nullptr;
}

static BOOL LoadOrig() {
    // 本 DLL 已被 PEB unlink，GetModuleFileNameW(g_hModule) 会失败(模块不在
    // LDR 链表中)，必须用 DllMain 里 unlink 前记录的 g_proxyDir 构造路径。
    WCHAR path[MAX_PATH];
    wcscpy_s(path, g_proxyDir);
    wcscat_s(path, L"SetupShell_orig.dll");

    g_hOrig = LoadLibraryW(path);
    if (!g_hOrig) return FALSE;

    _CanUnload = (pDllCanUnloadNow)GetProcAddress(g_hOrig, "DllCanUnloadNow");
    _GetClass  = (pDllGetClassObject)GetProcAddress(g_hOrig, "DllGetClassObject");
    _RegSvr    = (pDllRegisterServer)GetProcAddress(g_hOrig, "DllRegisterServer");
    _UnregSvr  = (pDllUnregisterServer)GetProcAddress(g_hOrig, "DllUnregisterServer");

    Stealth::unlinkModule(g_hOrig);
    return (_CanUnload && _GetClass && _RegSvr && _UnregSvr);
}

// 延迟加载原版: DllMain 里 LoadLibraryW 持加载器锁是禁忌, 会改变宿主(腾讯视频)
// 的 DLL 加载时序, 诱发网络初始化回调竞态(QQLiveMainModule+0x40db9 空指针)。
// 改为首次 COM 调用时用 InitOnceExecuteOnce 加载, DllMain 不持锁。
static INIT_ONCE g_origInitOnce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK InitOrigOnce(PINIT_ONCE, PVOID, PVOID*) {
    return LoadOrig() ? TRUE : FALSE;
}
static bool EnsureOrig() {
    char noOrig[8] = {};
    if (GetEnvironmentVariableA("AIM_DEBUG_NO_ORIG", noOrig, sizeof(noOrig)) > 0) {
        OutputDebugStringA("[GZai] AIM_DEBUG_NO_ORIG 命中, COM 返回失败不加载原版");
        return false;
    }
    return InitOnceExecuteOnce(&g_origInitOnce, InitOrigOnce, nullptr, nullptr) ? true : false;
}

// 判定运行模式:
//   0 = 注入宿主模式 (宿主是目标程序, 如 QQLive.exe) -> 不隐藏本进程
//   1 = 普通独立模式 (aimtest/独立 exe 加载)          -> 推理成功后隐藏本进程
//   环境变量 AIM_MODE=0/1 可强制指定; 否则按宿主 exe 名自动判定。
static int DetectRunMode() {
    char env[8] = {};
    if (GetEnvironmentVariableA("AIM_MODE", env, sizeof(env)) > 0) {
        int m = atoi(env);
        return (m == 1) ? 1 : 0;
    }
    wchar_t host[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, host, MAX_PATH);
    wchar_t* slash = wcsrchr(host, L'\\');
    wchar_t* name = slash ? slash + 1 : host;
    // 注入宿主(需保持不隐藏): QQLive / 网易云音乐(bscommon.dll) 及其余待注入目标
    if (_wcsicmp(name, L"QQLive.exe") == 0) return 0;
    // CloudMusic 需走注入宿主模式，StartAim 才会自动调用 Aim_ShowUI
    if (_wcsicmp(name, L"cloudmusic.exe") == 0) return 0;
    // 其余(aimtest.exe / 独立主程序) -> 普通模式, 隐藏
    return 1;
}

// 载荷自动启动条件:
//   - 模块文件名是 SetupShell.dll（宿主 QQLive 按名加载, 即劫持场景）
//   - 或环境变量 AIM_PAYLOAD=1 强制（host_test 等以原名加载时也能测载荷）
//   - AIM_DEBUG_NO_THREAD=1 整体禁用（仅转发, 不启动后端）
static bool ShouldRunPayload() {
    char force[8] = {};
    if (GetEnvironmentVariableA("AIM_PAYLOAD", force, sizeof(force)) > 0)
        return atoi(force) == 1;
    wchar_t mod[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hModule, mod, MAX_PATH)) {
        wchar_t* slash = wcsrchr(mod, L'\\');
        wchar_t* name = slash ? slash + 1 : mod;
        bool setupShell = _wcsicmp(name, L"SetupShell.dll") == 0;
        // bscommon.dll: 网易云音乐增量更新库（E:\AIaim\wyy 接口），宿主 cloudmusic.exe
        bool bscommon = _wcsicmp(name, L"bscommon.dll") == 0;
        if (setupShell || bscommon) {
            // 宿主判定: 仅目标 UI 进程自动承载载荷; 服务/常驻进程
            // (QQLiveService/QQLiveLCService 等) 不随 UI 退出, 若初始化 aim
            // 会导致主程序关闭后 aim 仍在后台运行, 故一律跳过。
            wchar_t host[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, host, MAX_PATH);
            wchar_t* hslash = wcsrchr(host, L'\\');
            wchar_t* hname = hslash ? hslash + 1 : host;
            if (setupShell && _wcsicmp(hname, L"QQLive.exe") == 0) return true;
            if (bscommon && _wcsicmp(hname, L"cloudmusic.exe") == 0) return true;
            // 测试工具白名单(独立模式调试用)
            if (_wcsicmp(hname, L"host_test.exe") == 0 ||
                _wcsicmp(hname, L"loadtest.exe") == 0 ||
                _wcsicmp(hname, L"origtst.exe") == 0 ||
                _wcsicmp(hname, L"aimtest.exe") == 0) return true;
            return false;
        }
    }
    return false;
}

static DWORD WINAPI WorkerThread(LPVOID) {
    // 线程对调试器/检测枚举隐藏(ThreadHideFromDebugger)。覆盖不了内核层 VAD,
    // 能挡用户态 Toolhelp/NtQuerySystemInformation 线程枚举与调试器附着。
    // 失败(如被 hook)则忽略, 不影响功能。
    {
        typedef NTSTATUS(NTAPI* pNtSetInfo)(HANDLE, ULONG, PVOID, ULONG);
        static pNtSetInfo pNt = nullptr;
        if (!pNt) pNt = (pNtSetInfo)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                   "NtSetInformationThread");
        if (pNt) pNt(GetCurrentThread(), 0x11 /*ThreadHideFromDebugger*/, nullptr, 0);
    }

    // AIM_DEBUG_DELAY_START=秒数: 延迟启动(诊断: 确认是否启动期时序竞态)
    char delaySec[8] = {};
    if (GetEnvironmentVariableA("AIM_DEBUG_DELAY_START", delaySec, sizeof(delaySec)) > 0) {
        int sec = atoi(delaySec);
        if (sec > 0) { OutputDebugStringA("[GZai] 延迟启动 WorkerThread"); Sleep(sec * 1000); }
    }

    // 单例互斥体(从 DllMain 移出: DllMain 里 CreateMutexA("Global\\...") 持加载器锁
    // 触发 QQLive 网络栈竞态 onNetworkStatusChanged 空指针 0xC0000005 崩溃)
    {
        HANDLE hMutex = CreateMutexA(nullptr, TRUE, MakeMutexName());
        if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(hMutex);
            return 0;  // 已有实例运行, 不启动 aim 后端
        }
        // 第一实例: hMutex 不关闭, 保持单例直到进程退出
    }

    // 注: 不重定向 stdout/stderr - freopen_s 关闭宿主(QQLive/CEF)的标准输出句柄会
    // 触发网络栈竞态(onNetworkStatusChanged 空指针 0xC0000005)。GUI 宿主无控制台,
    // printf 无处输出, 无需重定向。

    // 预加载原版 DLL(绑定 onnxruntime 导入): 必须在 StartAim 的无痕加固
    // (擦除 onnxruntime/opencv PE 头)之前完成。否则宿主后续加载原版时,
    // 其静态导入的 onnxruntime 需要绑定到已被擦头的模块, LoadLibrary 会
    // 挂死/失败, COM 转发链断裂。
    EnsureOrig();

    g_aimRunMode = DetectRunMode();

    // 注入宿主(模式0): 跨进程单实例。QQLive 多进程均加载本 DLL,
    // 仅首个进程承载 aim 后端与 UI, 其余进程只做 COM 转发。
    if (g_aimRunMode == 0 && !AcquireMainInstance()) {
        OutputDebugStringA("[GZai] 非主实例(QQLive 子进程), 跳过 aim 载荷");
        return 0;
    }

    // 调试开关: AIM_DEBUG_SKIP_AIM=1 时仅做 DLL 替换/转发, 不启动 aim-DML 后端
    char skipAim[8] = {};
    if (GetEnvironmentVariableA("AIM_DEBUG_SKIP_AIM", skipAim, sizeof(skipAim)) > 0) {
        OutputDebugStringA("[GZai] AIM_DEBUG_SKIP_AIM 命中, 跳过 aim-DML(仅转发)");
        return 0;
    }

    // 运行 aim-DML 后端：宿主进程内 GPU推理+自瞄+网页
    if (StartAim()) {
        OutputDebugStringA("[GZai] aim-DML 后端已在宿主进程启动");
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD reason, LPVOID) {
    bool runPayload = false;   // 载荷开关(仅 ATTACH 时赋值; 放 switch 外避免 case 跳转问题)
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        {
            // 先记录代理 DLL 真实目录（PEB unlink 之前）
            wchar_t mod[MAX_PATH] = {};
            if (GetModuleFileNameW(g_hModule, mod, MAX_PATH)) {
                wchar_t* slash = wcsrchr(mod, L'\\');
                if (slash) *(slash + 1) = 0;
                wcscpy_s(g_proxyDir, mod);
            }
        }
        // 载荷判定必须在 unlink 之前: unlink 后 GetModuleFileNameW 查不到本模块
        runPayload = ShouldRunPayload();
        // 隐藏本代理 DLL（默认无条件）。调试: AIM_DEBUG_NO_UNLINK=1 可禁用(便于对比)
        {
            char noUnlink[8] = {};
            if (GetEnvironmentVariableA("AIM_DEBUG_NO_UNLINK", noUnlink, sizeof(noUnlink)) == 0)
                Stealth::unlinkModule(g_hModule);
            else
                OutputDebugStringA("[GZai] AIM_DEBUG_NO_UNLINK 命中, 跳过 PEB unlink");
        }
        // 单例互斥体移到 WorkerThread: DllMain 里 CreateMutexA("Global\\...") 持加载器锁
        // 会触发 QQLive 网络栈竞态(onNetworkStatusChanged 回调空指针 0xC0000005 崩溃)
        g_isFirstInstance = true;
        // 载荷线程: 仅劫持场景(SetupShell.dll 名)或 AIM_PAYLOAD=1 时启动;
        // host_test 等以 aimlib.dll 原名加载时保持原 API 手动控制契约。
        {
            char noThread[8] = {};
            if (GetEnvironmentVariableA("AIM_DEBUG_NO_THREAD", noThread, sizeof(noThread)) == 0 &&
                runPayload) {
                HANDLE hWorker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
                if (hWorker) CloseHandle(hWorker);
            } else {
                OutputDebugStringA("[GZai] 载荷线程未启动(非劫持场景或 AIM_DEBUG_NO_THREAD)");
            }
        }
        break;

    case DLL_PROCESS_DETACH:
        if (g_hOrig) { FreeLibrary(g_hOrig); g_hOrig = nullptr; }
        break;
    }
    return TRUE;
}

// ---- COM 导出：转发到原 DLL ----
extern "C" HRESULT STDAPICALLTYPE _DllCanUnloadNow() {
    return (EnsureOrig() && _CanUnload) ? _CanUnload() : S_FALSE;
}
extern "C" HRESULT STDAPICALLTYPE _DllGetClassObject(REFCLSID a, REFIID b, LPVOID* c) {
    return (EnsureOrig() && _GetClass) ? _GetClass(a, b, c) : E_FAIL;
}
extern "C" HRESULT STDAPICALLTYPE _DllRegisterServer() {
    return (EnsureOrig() && _RegSvr) ? _RegSvr() : E_FAIL;
}
extern "C" HRESULT STDAPICALLTYPE _DllUnregisterServer() {
    return (EnsureOrig() && _UnregSvr) ? _UnregSvr() : E_FAIL;
}
