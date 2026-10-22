#include "Common.h"

LONGLONG QpcNow100ns()
{
    static const LONGLONG freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);

    const LONGLONG whole = (c.QuadPart / freq) * 10000000LL;
    const LONGLONG part = (c.QuadPart % freq) * 10000000LL / freq;
    return whole + part;
}

namespace {

struct HrEntry
{
    unsigned long hr;
    const wchar_t* name;
};

const HrEntry kHrNames[] = {
    { 0x80004005UL, L"E_FAIL" },
    { 0x80070005UL, L"E_ACCESSDENIED" },
    { 0x8007001FUL, L"ERROR_GEN_FAILURE" },
    { 0x800700AAUL, L"ERROR_BUSY" },
    { 0x80070490UL, L"ERROR_NOT_FOUND" },
    { 0x800401F0UL, L"CO_E_NOTINITIALIZED" },
    { 0xC00D36B0UL, L"MF_E_PLATFORM_NOT_INITIALIZED" },
    { 0xC00D36B2UL, L"MF_E_INVALIDREQUEST" },
    { 0xC00D36B3UL, L"MF_E_INVALIDSTREAMNUMBER" },
    { 0xC00D36B4UL, L"MF_E_INVALIDMEDIATYPE (设备当前给不出可用格式: 常见于无输入信号或被别的程序占着)" },
    { 0xC00D36B5UL, L"MF_E_NOTACCEPTING" },
    { 0xC00D36B6UL, L"MF_E_NOT_INITIALIZED" },
    { 0xC00D36B7UL, L"MF_E_UNSUPPORTED_REPRESENTATION" },
    { 0xC00D36B9UL, L"MF_E_NO_MORE_TYPES" },
    { 0xC00D36BAUL, L"MF_E_UNSUPPORTED_SERVICE" },
    { 0xC00D36BBUL, L"MF_E_UNEXPECTED" },
    { 0xC00D36E3UL, L"MF_E_SHUTDOWN" },
    { 0xC00D3E85UL, L"MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED" },
    { 0xC00D3EA2UL, L"MF_E_HW_MFT_FAILED_START_STREAMING" },
    { 0xC00DABE8UL, L"MF_E_VIDEO_RECORDING_DEVICE_LOCKED (设备被其它程序独占)" },
};

}

std::wstring HrText(HRESULT hr)
{
    wchar_t head[32];
    swprintf_s(head, L"0x%08X", (unsigned)hr);
    for (const HrEntry& e : kHrNames) {
        if (e.hr == (unsigned long)hr)
            return std::wstring(head) + L" " + e.name;
    }

    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, (DWORD)hr, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);

    std::wstring out = head;
    if (n > 0 && msg) {
        std::wstring text(msg, n);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                                 text.back() == L' '))
            text.pop_back();
        if (!text.empty())
            out += L" " + text;
    }
    if (msg)
        LocalFree(msg);
    return out;
}
