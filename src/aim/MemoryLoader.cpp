// ============================================================
//  最小化 PE 内存加载器（x64）—— 实现
//  只做本 DLL 需要的：校验 -> 分配 -> 复制区段 -> 重定位 -> 导入 -> 入口
// ============================================================
#include "MemoryLoader.h"
#include <cstring>
#include <cstdint>

static inline const IMAGE_NT_HEADERS* NtOf(const void* img) {
    const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(img);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    return reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(img) + dos->e_lfanew);
}

// ---- .pdata 异常展开表注册 ----
// x64 C++ 异常展开依赖 RUNTIME_FUNCTION 表；内存加载的模块不注册则
// 模块内 throw/catch 展开时查不到 unwind 信息 → 崩溃。每个模块一份，
// 链表管理，SRW 锁保护（封装dll 与 NeuralCurveModule 可能同时驻留）。
struct PdataReg {
    uintptr_t    base;
    PRUNTIME_FUNCTION table;
    DWORD        count;
    PdataReg*    next;
};
static PdataReg* g_pdataRegs = nullptr;
static SRWLOCK  g_pdataLock = SRWLOCK_INIT;

static void RegisterPdata(uintptr_t base, const IMAGE_NT_HEADERS* nt) {
    const auto& exc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!exc.VirtualAddress || !exc.Size) return;
    DWORD count = exc.Size / sizeof(RUNTIME_FUNCTION);
    if (!count) return;
    auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(base + exc.VirtualAddress);
    if (!RtlAddFunctionTable(table, count, base)) return;
    AcquireSRWLockExclusive(&g_pdataLock);
    g_pdataRegs = new PdataReg{base, table, count, g_pdataRegs};
    ReleaseSRWLockExclusive(&g_pdataLock);
}

static void UnregisterPdata(uintptr_t base) {
    AcquireSRWLockExclusive(&g_pdataLock);
    PdataReg** pp = &g_pdataRegs;
    while (*pp) {
        if ((*pp)->base == base) {
            PdataReg* d = *pp;
            *pp = d->next;
            RtlDeleteFunctionTable(d->table);
            delete d;
            break;
        }
        pp = &(*pp)->next;
    }
    ReleaseSRWLockExclusive(&g_pdataLock);
}

HMODULE MemLoadLibrary(const void* image, SIZE_T size) {
    if (!image || size < sizeof(IMAGE_DOS_HEADER)) return nullptr;
    const auto* nt = NtOf(image);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return nullptr;
    if (nt->OptionalHeader.SizeOfImage == 0) return nullptr;

    // 分配映像（RWX：简化处理，实际区段保护不精确还原）
    uintptr_t base = reinterpret_cast<uintptr_t>(VirtualAlloc(
        nullptr, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!base) return nullptr;

    // 复制头部（保证 DOS/NT/区段头在内存中有效）
    SIZE_T headersSize = nt->OptionalHeader.SizeOfHeaders;
    if (headersSize > size) headersSize = size;
    memcpy(reinterpret_cast<void*>(base), image, headersSize);

    // 复制各区段
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!sec[i].SizeOfRawData) continue;
        SIZE_T copySize = sec[i].SizeOfRawData;
        if (copySize > sec[i].Misc.VirtualSize) copySize = sec[i].Misc.VirtualSize;
        memcpy(reinterpret_cast<void*>(base + sec[i].VirtualAddress),
               reinterpret_cast<const uint8_t*>(image) + sec[i].PointerToRawData,
               copySize);
    }

    // 重定位
    uintptr_t delta = base - nt->OptionalHeader.ImageBase;
    if (delta != 0) {
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (dir.VirtualAddress && dir.Size) {
            uint8_t* cur = reinterpret_cast<uint8_t*>(base + dir.VirtualAddress);
            uint8_t* end = cur + dir.Size;
            while (cur + sizeof(IMAGE_BASE_RELOCATION) <= end) {
                const auto* blk = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(cur);
                if (blk->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
                SIZE_T count = (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                const auto* entries = reinterpret_cast<const uint16_t*>(cur + sizeof(IMAGE_BASE_RELOCATION));
                for (SIZE_T i = 0; i < count; i++) {
                    if ((entries[i] >> 12) == IMAGE_REL_BASED_DIR64) {
                        auto* addr = reinterpret_cast<uintptr_t*>(base + blk->VirtualAddress + (entries[i] & 0xFFF));
                        *addr += delta;
                    }
                }
                cur += blk->SizeOfBlock;
            }
        }
    }

    // 导入表
    const auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress) {
        auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + impDir.VirtualAddress);
        for (; imp->Name; imp++) {
            const char* dllName = reinterpret_cast<const char*>(base + imp->Name);
            HMODULE hDll = LoadLibraryA(dllName);
            if (!hDll) continue; // 单个导入 DLL 缺失不阻断加载
            auto* thunk = reinterpret_cast<uintptr_t*>(base + imp->FirstThunk);
            const auto* orig = imp->OriginalFirstThunk
                ? reinterpret_cast<uintptr_t*>(base + imp->OriginalFirstThunk)
                : thunk;
            for (; *orig; orig++, thunk++) {
                FARPROC p;
                if (*orig & IMAGE_ORDINAL_FLAG64) {
                    p = GetProcAddress(hDll, reinterpret_cast<LPCSTR>(*orig & 0xFFFF));
                } else {
                    const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + *orig);
                    p = GetProcAddress(hDll, reinterpret_cast<LPCSTR>(byName->Name));
                }
                *thunk = reinterpret_cast<uintptr_t>(p);
            }
        }
    }

    // 注册 .pdata 异常展开表（在入口点之前：DllMain 内 throw 也能展开）
    RegisterPdata(base, nt);

    // 调用入口点（DLL_PROCESS_ATTACH）
    if (nt->OptionalHeader.AddressOfEntryPoint) {
        using EntryFn = BOOL(WINAPI*)(HMODULE, DWORD, LPVOID);
        auto entry = reinterpret_cast<EntryFn>(base + nt->OptionalHeader.AddressOfEntryPoint);
        entry(reinterpret_cast<HMODULE>(base), DLL_PROCESS_ATTACH, nullptr);
    }
    return reinterpret_cast<HMODULE>(base);
}

FARPROC MemGetProcAddress(HMODULE hModule, const char* name) {
    uintptr_t base = reinterpret_cast<uintptr_t>(hModule);
    const auto* nt = NtOf(reinterpret_cast<void*>(base));
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto& expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!expDir.VirtualAddress) return nullptr;
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + expDir.VirtualAddress);
    if (!exp->AddressOfNames || !exp->NumberOfNames) return nullptr;

    const auto* names = reinterpret_cast<const uint32_t*>(base + exp->AddressOfNames);
    const auto* ords = reinterpret_cast<const uint16_t*>(base + exp->AddressOfNameOrdinals);
    const auto* funcs = reinterpret_cast<const uint32_t*>(base + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        const char* n = reinterpret_cast<const char*>(base + names[i]);
        if (strcmp(n, name) == 0) {
            uint32_t rva = funcs[ords[i]];
            return reinterpret_cast<FARPROC>(base + rva);
        }
    }
    return nullptr;
}

void MemFreeLibrary(HMODULE hModule) {
    uintptr_t base = reinterpret_cast<uintptr_t>(hModule);
    const auto* nt = NtOf(reinterpret_cast<void*>(base));
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return;
    if (nt->OptionalHeader.AddressOfEntryPoint) {
        using EntryFn = BOOL(WINAPI*)(HMODULE, DWORD, LPVOID);
        auto entry = reinterpret_cast<EntryFn>(base + nt->OptionalHeader.AddressOfEntryPoint);
        entry(reinterpret_cast<HMODULE>(base), DLL_PROCESS_DETACH, nullptr);
    }
    UnregisterPdata(base);
    VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
}
