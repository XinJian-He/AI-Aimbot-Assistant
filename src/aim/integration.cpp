// ============================================================
//  integration.cpp — aim-DML 后端集成层实现
//  在宿主进程(腾讯视频)内：预加载运行时库 -> 设置模型路径 ->
//  AimBackend::Init -> 启动自瞄推理。
//  原参考实现的卡密(Licensing)已裁掉：无卡密直接进入；
//  运行模式/无痕加固/崩溃落盘逻辑保留。
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include "integration.h"
#include "AimBackend.h"
#include "stealth/stealth.h"
#include <windows.h>
#include <string>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdarg>

// 代理 DLL 所在目录（与 SetupShell.dll 同目录，运行时库/模型放这里）
// 注意: 代理 DLL 会被 PEB unlink，GetModuleHandleExW(FROM_ADDRESS) 找不到它，
//       因此使用 proxy.cpp 在 DllMain 里记录的	g_proxyDir（unlink 前的真实路径）。
extern "C" wchar_t g_proxyDir[MAX_PATH];
// 运行模式: 0=注入宿主(不隐藏自进程) 1=普通独立(推理成功后隐藏自进程)
// 定义在 proxy.cpp, WorkerThread 调用 StartAim 前赋值; 此处仅供声明
extern "C" int g_aimRunMode;
static std::wstring SelfDirOf(void* addr) {
    if (g_proxyDir[0]) return std::wstring(g_proxyDir);
    wchar_t mod[MAX_PATH] = {};
    HMODULE h = nullptr;
    if (addr)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &h);
    if (!h) h = GetModuleHandleW(nullptr);
    if (!h || !GetModuleFileNameW(h, mod, MAX_PATH))
        GetModuleFileNameW(nullptr, mod, MAX_PATH);
    std::wstring s(mod);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? s : s.substr(0, p + 1);
}

// 调试日志: 写 可写目录\aim_log.txt(代理 DLL 同目录可能只读, 落盘到这里)
static void Log(const char* fmt, ...) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"aim_log.txt";
    FILE* f = _wfopen(path.c_str(), L"a");
    if (f) {
        va_list a; va_start(a, fmt);
        vfprintf(f, fmt, a);
        fprintf(f, "\n");
        va_end(a);
        fclose(f);
    }
}

// Native UI uses its own rendering thread; startup mode remains unchanged.
extern "C" __declspec(dllexport) int __cdecl Aim_ShowUI(void);
extern "C" __declspec(dllexport) void __cdecl Aim_CloseUI(void);
extern "C" __declspec(dllexport) int __cdecl Aim_LoadConfigW(const wchar_t* path);
static void AutoStartUI() { Log("[integration] native UI ready: %d", Aim_ShowUI()); }

// 预加载运行时库(onnxruntime/DirectML/opencv)，与代理 DLL 同目录。
// aimlib.dll 静态导入 opencv/onnxruntime，加载时已按模块目录解析；
// 这里再显式 LoadLibrary 兜底(DLL 目录可能不在系统搜索路径)，
// DirectML 则由 onnxruntime 内部按名加载，需先在此就位。
static HMODULE g_hOrt = nullptr;
static HMODULE g_hOpenCV = nullptr;
static HMODULE g_hDML = nullptr;

static void EnsureRuntimeDlls() {
    std::wstring dir = SelfDirOf((void*)&EnsureRuntimeDlls);
    SetDllDirectoryW(dir.c_str());
    g_hOrt = LoadLibraryW((dir + L"onnxruntime.dll").c_str());
    g_hDML = LoadLibraryW((dir + L"DirectML.dll").c_str());
    // 本工程部署 opencv 4.12(4120)；老版本 4.11(4110) 兜底
    g_hOpenCV = LoadLibraryW((dir + L"opencv_world4120.dll").c_str());
    if (!g_hOpenCV) g_hOpenCV = LoadLibraryW((dir + L"opencv_world4110.dll").c_str());
}

// 无痕加固: 加载完成(含 ORT 会话建立/AimBackend::Init 之后)调用。
//  - onnxruntime/opencv: PEB 模块链摘除 + BaseDllName/FullDllName 清零 + 擦除 PE 头
//    (memset 0x1000), 使内存态不再出现完整带名 PE, 阻断 VAD/内存 dump 识别。
//  - DirectML: 不碰。onnxruntime 内部按名调用 DirectML 且依赖其自身 PE 头
//    做区域分配/资源释放, 擦头/unlink 会导致 DML 运行动态链接或释放失败,
//    需先经 AIM_DEBUG_NO_HARDEN 验证后再单独决定。
// 环境变量 AIM_DEBUG_NO_HARDEN=1 可整体禁用(便于对比验证)。
static void HardenRuntimeModules() {
    char noHarden[8] = {};
    if (GetEnvironmentVariableA("AIM_DEBUG_NO_HARDEN", noHarden, sizeof(noHarden)) > 0) {
        Log("[integration] AIM_DEBUG_NO_HARDEN 命中, 跳过运行库加固");
        return;
    }
    bool ok = false;
    if (g_hOrt) {
        Stealth::unlinkModule(g_hOrt);
        Stealth::erasePEHeader(g_hOrt);
        ok = true;
    }
    if (g_hOpenCV) {
        Stealth::unlinkModule(g_hOpenCV);
        Stealth::erasePEHeader(g_hOpenCV);
        ok = true;
    }
    if (ok)
        Log("[integration] 运行库加固完成: onnxruntime/opencv 已 unlink+擦头(不碰 DirectML)");
}

// 向量异常处理: 捕获致命异常(访问违规等)落盘, 确认崩溃线程归属(代理 DLL 是否涉案)。
// 不吞异常(EXCEPTION_CONTINUE_SEARCH), 让宿主崩溃处理继续, 仅做观察记录。
static LONG NTAPI AimVectoredHandler(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // 过滤非致命: C++ 异常 / 调试输出 / OutputDebugString 等
    if (code == 0xE06D7363 /*C++ exception*/ || code == DBG_PRINTEXCEPTION_C ||
        (code & 0xF0000000) == 0x10000000)
        return EXCEPTION_CONTINUE_SEARCH;
    // 仅记录致命: 访问违规 / 栈溢出 / fast-fail
    if (code != 0xC0000005 && code != 0xC00000FD && code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"aim_crash.txt";
    FILE* f = _wfopen(path.c_str(), L"a");
    if (f) {
        HMODULE hmod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &hmod);
        char modName[MAX_PATH] = "(unknown)";
        if (hmod) GetModuleFileNameA(hmod, modName, MAX_PATH);
        fprintf(f, "[CRASH] code=0x%08lX addr=%p tid=%lu pid=%lu\n        mod=%s\n",
            (unsigned long)code, ep->ExceptionRecord->ExceptionAddress,
            (unsigned long)GetCurrentThreadId(), (unsigned long)GetCurrentProcessId(), modName);
        fflush(f);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool StartAim() {
    static bool vehInstalled = false;
    if (!vehInstalled) {
        AddVectoredExceptionHandler(1, AimVectoredHandler);
        vehInstalled = true;
        Log("[integration] VEH 已安装(致命异常落盘 aim_crash.txt)");
    }
    Log("[integration] StartAim 进入");
    try {
        Log("[integration] 运行模式: %s",
            g_aimRunMode == 0 ? "注入QQLive(不隐藏)" : "普通独立(隐藏)");
        // 注: 参考实现在此置 g_autoHideOnInfer=(模式==1) 由后端推理成功后
        //     隐藏自进程; aimlib 后端无此开关, 进程隐藏需由宿主/注入器负责。

        EnsureRuntimeDlls();
        Log("[integration] StartAim: 运行时库已预加载");

        std::wstring dir = SelfDirOf((void*)&StartAim);
        Log("[integration] 代理目录: [%ls]", dir.c_str());

        // 恢复上次调参结果：加载 代理 DLL 同目录 配置\aim_config.ini。
        // 没有这一步时重启后所有参数回到代码默认值，网页里调过的
        // 模型/移动曲线/预览窗口等用户设置全部丢失。
        std::wstring cfgPath = dir + L"配置\\aim_config.ini";
        if (GetFileAttributesW(cfgPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (Aim_LoadConfigW(cfgPath.c_str()))
                Log("[integration] 已加载配置: %ls", cfgPath.c_str());
        }

        // 模型路径: 配置里的路径优先；为空或文件不存在时才兜底
        // 代理 DLL 同目录 models\wa_v8_256.onnx（再 model\、同目录根）
        bool modelOk = g_modelPath[0] != '\0';
        if (modelOk) {
            int wl = MultiByteToWideChar(CP_UTF8, 0, g_modelPath, -1, nullptr, 0);
            std::wstring mp((size_t)wl, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, g_modelPath, -1, &mp[0], wl);
            modelOk = GetFileAttributesW(mp.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
        if (!modelOk) {
            std::wstring modelW = dir + L"models\\wa_v8_256.onnx";
            if (GetFileAttributesW(modelW.c_str()) == INVALID_FILE_ATTRIBUTES)
                modelW = dir + L"model\\wa_v8_256.onnx";
            if (GetFileAttributesW(modelW.c_str()) == INVALID_FILE_ATTRIBUTES)
                modelW = dir + L"wa_v8_256.onnx";
            int n = WideCharToMultiByte(CP_UTF8, 0, modelW.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (n > 0) {
                std::string model(n - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, modelW.c_str(), -1, &model[0], n, nullptr, nullptr);
                strncpy(g_modelPath, model.c_str(), sizeof(g_modelPath) - 1);
            }
        }
        Log("[integration] 模型路径: %s", g_modelPath);

        // 初始化后端(截图/推理/控制线程; 默认暂停)
        AimBackend::Get().Init();
        Log("[integration] AimBackend::Init 完成");

        // 无痕加固: ORT 会话已建立, 延迟加载已解析, 此时擦头/unlink 运行库
        // (onnxruntime/opencv; DirectML 保留)。迟于此点调用可避免破坏 delay-load。
        HardenRuntimeModules();

        // 自动启动原生 ImGui 控制台(仅注入宿主模式; 独立模式由宿主/测试工具决定)
        if (g_aimRunMode == 0) {
            AutoStartUI();
            Log("[integration] 已请求启动原生控制台");
        }

        // 默认不自动开始推理: 初始保持暂停, 由宿主调用 Aim_Start() 或
        // 环境变量 AIM_AUTOSTART=1 自动开始(模拟原生面板「开始」按钮)。
        char autostart[8] = {};
        if (GetEnvironmentVariableA("AIM_AUTOSTART", autostart, sizeof(autostart)) > 0 &&
            autostart[0] == '1') {
            AimBackend::Get().RequestStart();
            Log("[integration] AIM_AUTOSTART=1, 已请求开始推理");
        } else {
            Log("[integration] 初始暂停, 待宿主 Aim_Start()/AIM_AUTOSTART=1 触发开始");
        }
        return true;
    }
    catch (const std::exception& e) {
        Log("[integration] C++异常: %s", e.what());
        return false;
    }
    catch (...) {
        Log("[integration] 未知异常");
        return false;
    }
}

void StopAim() {
    Aim_CloseUI();
    AimBackend::Get().Shutdown();
}
