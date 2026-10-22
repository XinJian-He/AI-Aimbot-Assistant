// ============================================================
//  bscommon_relay.cpp — aimlib.dll 导出接口（对齐 E:\AIaim\wyy）
//
//  导出（序号与 wyy\bscommon.def 一致）：
//    @1 int __cdecl bsdiff  (const char*    oldfile, const char*    newfile,
//                            const char*    patchfile)
//    @2 int __cdecl bspatchW(const wchar_t* oldfile, const wchar_t* patchfile,
//                            const wchar_t* newfile)
//
//  首次调用时懒加载同目录 bscommon_orig.dll，并从其中取同名函数转发；
//  环境变量 BSCOMMON_ORIG 可指定原始 DLL 的完整路径。
//  原始 DLL 缺失或取址失败时返回 -1（与 wyy 中转实现一致）。
//
//  目录来源：proxy.cpp 的 DllMain 在 PEB unlink 之前把本 DLL 所在目录
//  记录到 g_proxyDir；模块被 unlink 后 GetModuleFileNameW 不可用，故优先
//  使用该缓存，仅在为空时回退到模块地址查询。
// ============================================================
#include <windows.h>
#include <cwchar>

#ifndef ORIG_DLL_NAME
#define ORIG_DLL_NAME L"bscommon_orig.dll"
#endif

typedef int (__cdecl* PFN_BSDIFF)(const char*, const char*, const char*);
typedef int (__cdecl* PFN_BSPATCHW)(const wchar_t*, const wchar_t*, const wchar_t*);

// proxy.cpp 中的 selectany 弱定义（DllMain 写入，供全模块共享）
extern "C" wchar_t g_proxyDir[MAX_PATH];

static SRWLOCK       g_lock = SRWLOCK_INIT;
static bool          g_initialized = false;
static HMODULE       g_origModule = nullptr;
static PFN_BSDIFF    g_origBsdiff = nullptr;
static PFN_BSPATCHW  g_origBspatchW = nullptr;

static bool ModuleDirOfSelf(wchar_t* dir, DWORD cch) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&g_lock), &module) ||
        !module) {
        return false;
    }
    DWORD n = GetModuleFileNameW(module, dir, cch);
    if (n == 0 || n >= cch) return false;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';
    return true;
}

static void EnsureOriginalLocked() {
    if (g_initialized) return;
    g_initialized = true;

    wchar_t path[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"BSCOMMON_ORIG", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        path[0] = L'\0';
        if (g_proxyDir[0] != L'\0') {
            wcscpy_s(path, g_proxyDir);
            wcscat_s(path, ORIG_DLL_NAME);
        } else {
            // 模块已被 PEB unlink 时此处通常失败，仅作兜底。
            if (ModuleDirOfSelf(path, MAX_PATH))
                wcscat_s(path, ORIG_DLL_NAME);
            else
                path[0] = L'\0';
        }
    }

    if (path[0] == L'\0') {
        OutputDebugStringA("[GZai] bscommon relay: cannot resolve original DLL path");
        return;
    }

    g_origModule = LoadLibraryW(path);
    if (!g_origModule) {
        OutputDebugStringA("[GZai] bscommon relay: LoadLibraryW(original) failed");
        return;
    }

    g_origBsdiff =
        reinterpret_cast<PFN_BSDIFF>(GetProcAddress(g_origModule, "bsdiff"));
    g_origBspatchW =
        reinterpret_cast<PFN_BSPATCHW>(GetProcAddress(g_origModule, "bspatchW"));
}

static void EnsureOriginal() {
    AcquireSRWLockExclusive(&g_lock);
    EnsureOriginalLocked();
    ReleaseSRWLockExclusive(&g_lock);
}

extern "C" int __cdecl bsdiff(const char* oldfile, const char* newfile,
                              const char* patchfile) {
    EnsureOriginal();
    return g_origBsdiff ? g_origBsdiff(oldfile, newfile, patchfile) : -1;
}

extern "C" int __cdecl bspatchW(const wchar_t* oldfile, const wchar_t* patchfile,
                                const wchar_t* newfile) {
    EnsureOriginal();
    return g_origBspatchW ? g_origBspatchW(oldfile, patchfile, newfile) : -1;
}
