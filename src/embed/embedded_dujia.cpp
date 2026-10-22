// ============================================================
//  exe 内嵌 封装dll 的实现
//  流程: 解密 kDujiaEnc（XOR 流密钥, 与 pack_dujia.py 一致）
//        -> MemLoadLibrary 内存加载（不写磁盘）
//        -> DujiaProc 按名查导出
// ============================================================
#include "embedded_dujia.h"
#include "embedded_dujia_data.h"
#include "MemoryLoader.h"
#include <cstdint>

// ---- 密钥派生：与 pack_dujia.py derive_key() 完全一致 ----
static void DujiaDeriveKey(unsigned char* key, int n) {
    unsigned int seed = 0x9E3779B9u;
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        key[i] = static_cast<unsigned char>((seed >> 16) & 0xFF);
    }
}

static HMODULE g_dujiaMem = nullptr;

HMODULE DujiaLoad() {
    if (g_dujiaMem) return g_dujiaMem;

    const size_t encSize = sizeof(kDujiaEnc);
    HANDLE heap = GetProcessHeap();
    auto* buf = static_cast<unsigned char*>(HeapAlloc(heap, 0, encSize));
    if (!buf) return nullptr;

    unsigned char key[32];
    DujiaDeriveKey(key, sizeof(key));
    for (size_t i = 0; i < encSize; i++)
        buf[i] = kDujiaEnc[i] ^ key[i % sizeof(key)];

    g_dujiaMem = MemLoadLibrary(buf, encSize);
    HeapFree(heap, 0, buf); // 加载器已复制映像，解密缓冲立即释放
    return g_dujiaMem;
}

FARPROC DujiaProc(const char* name) {
    if (g_dujiaMem) {
        FARPROC p = MemGetProcAddress(g_dujiaMem, name);
        if (p) return p;
    }
    // 兼容：外部目录的 封装dll.dll（LoadLibrary 加载的）
    HMODULE ext = GetModuleHandleW(L"封装dll.dll");
    if (ext && ext != g_dujiaMem) return GetProcAddress(ext, name);
    return nullptr;
}

void DujiaFree() {
    if (g_dujiaMem) {
        MemFreeLibrary(g_dujiaMem);
        g_dujiaMem = nullptr;
    }
}

bool DujiaIsEmbedded(HMODULE h) {
    return h != nullptr && h == g_dujiaMem;
}
