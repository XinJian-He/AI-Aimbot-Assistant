#include "stealth.h"
#include <winternl.h>
#include <cstring>
#include <intrin.h>

// winternl.h 只暴露 InMemoryOrderLinks，这里补全前 3 条链表字段
struct LDR_ENTRY_FULL {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID      DllBase;
    PVOID      EntryPoint;
    ULONG      SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
};

static PEB* getPEB() {
#if defined(_M_X64) || defined(__x86_64__)
    return reinterpret_cast<PEB*>(__readgsqword(0x60));
#else
    return reinterpret_cast<PEB*>(__readfsdword(0x30));
#endif
}

static void unlinkListEntry(LIST_ENTRY* entry) {
    entry->Blink->Flink = entry->Flink;
    entry->Flink->Blink = entry->Blink;
    entry->Flink = entry;
    entry->Blink = entry;
}

static LDR_ENTRY_FULL* findLdrEntry(HMODULE hModule) {
    PEB* peb = getPEB();
    auto head = &peb->Ldr->InMemoryOrderModuleList;
    for (auto entry = head->Flink; entry != head; entry = entry->Flink) {
        auto ldr = CONTAINING_RECORD(entry, LDR_ENTRY_FULL, InMemoryOrderLinks);
        if (ldr->DllBase == hModule) return ldr;
    }
    return nullptr;
}

static void wipeNames(LDR_ENTRY_FULL* ldr) {
    if (ldr->BaseDllName.Buffer) {
        memset(ldr->BaseDllName.Buffer, 0, ldr->BaseDllName.MaximumLength);
        ldr->BaseDllName.Length = 0;
    }
    if (ldr->FullDllName.Buffer) {
        memset(ldr->FullDllName.Buffer, 0, ldr->FullDllName.MaximumLength);
        ldr->FullDllName.Length = 0;
    }
}

void Stealth::unlinkModule(HMODULE hModule) {
    if (!hModule) hModule = GetModuleHandleA(nullptr);
    auto ldr = findLdrEntry(hModule);
    if (!ldr) return;
    unlinkListEntry(&ldr->InLoadOrderLinks);
    unlinkListEntry(&ldr->InMemoryOrderLinks);
    unlinkListEntry(&ldr->InInitializationOrderLinks);
    wipeNames(ldr);
}

void Stealth::unlinkModuleByName(const wchar_t* dllName) {
    PEB* peb = getPEB();
    auto head = &peb->Ldr->InMemoryOrderModuleList;
    size_t nameLen = wcslen(dllName);

    for (auto entry = head->Flink; entry != head; entry = entry->Flink) {
        auto ldr = CONTAINING_RECORD(entry, LDR_ENTRY_FULL, InMemoryOrderLinks);
        if (!ldr->BaseDllName.Buffer) continue;
        int n = ldr->BaseDllName.Length / sizeof(WCHAR);
        if (n < static_cast<int>(nameLen)) continue;
        bool match = true;
        for (size_t i = 0; i < nameLen; i++) {
            wchar_t a = ldr->BaseDllName.Buffer[i];
            wchar_t b = dllName[i];
            if (a >= L'A' && a <= L'Z') a += 32;
            if (b >= L'A' && b <= L'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) {
            unlinkListEntry(&ldr->InLoadOrderLinks);
            unlinkListEntry(&ldr->InMemoryOrderLinks);
            unlinkListEntry(&ldr->InInitializationOrderLinks);
            wipeNames(ldr);
            return;
        }
    }
}

void Stealth::erasePEHeader(HMODULE hModule) {
    if (!hModule) hModule = GetModuleHandleA(nullptr);
    DWORD oldProtect;
    if (VirtualProtect(hModule, 0x1000, PAGE_READWRITE, &oldProtect)) {
        memset(hModule, 0, 0x1000);
        VirtualProtect(hModule, 0x1000, oldProtect, &oldProtect);
    }
}
