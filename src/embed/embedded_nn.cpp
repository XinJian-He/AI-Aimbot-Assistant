// ============================================================
//  exe 内嵌 NeuralCurveModule 的实现
//  流程: 解密 kNnEnc（XOR 流密钥, 与 pack_dujia.py 一致, 种子 0x51F57A93）
//        -> MemLoadLibrary 内存加载（不写磁盘）
//        -> NnProc 按名查导出
// ============================================================
#include "embedded_nn.h"
#include "embedded_nn_data.h"
#include "MemoryLoader.h"
#include <cstdint>

// ---- 密钥派生：与 pack_dujia.py derive_key(seed=0x51F57A93) 完全一致 ----
static void NnDeriveKey(unsigned char* key, int n) {
    unsigned int seed = 0x51F57A93u;
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        key[i] = static_cast<unsigned char>((seed >> 16) & 0xFF);
    }
}

static HMODULE g_nnMem = nullptr;

HMODULE NnLoad() {
    if (g_nnMem) return g_nnMem;

    const size_t encSize = sizeof(kNnEnc);
    HANDLE heap = GetProcessHeap();
    auto* buf = static_cast<unsigned char*>(HeapAlloc(heap, 0, encSize));
    if (!buf) return nullptr;

    unsigned char key[32];
    NnDeriveKey(key, sizeof(key));
    for (size_t i = 0; i < encSize; i++)
        buf[i] = kNnEnc[i] ^ key[i % sizeof(key)];

    g_nnMem = MemLoadLibrary(buf, encSize);
    HeapFree(heap, 0, buf); // 加载器已复制映像，解密缓冲立即释放
    return g_nnMem;
}

FARPROC NnProc(const char* name) {
    if (g_nnMem) {
        FARPROC p = MemGetProcAddress(g_nnMem, name);
        if (p) return p;
    }
    // 兼容：外部目录的 NeuralCurveModule.dll（LoadLibrary 加载的）
    HMODULE ext = GetModuleHandleW(L"NeuralCurveModule.dll");
    if (ext && ext != g_nnMem) return GetProcAddress(ext, name);
    return nullptr;
}

void NnFree() {
    if (g_nnMem) {
        MemFreeLibrary(g_nnMem);
        g_nnMem = nullptr;
    }
}

bool NnIsEmbedded(HMODULE h) {
    return h != nullptr && h == g_nnMem;
}
