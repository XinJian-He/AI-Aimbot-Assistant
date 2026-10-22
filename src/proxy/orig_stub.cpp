#include <windows.h>

// 定义(combaseapi.h 已 extern "C" 声明)，导出由 orig_stub.def 控制
HRESULT STDAPICALLTYPE DllCanUnloadNow(void) {
    return S_OK;
}
HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID, REFIID, void** ppv) {
    if (ppv) *ppv = nullptr;
    return E_NOINTERFACE;
}
HRESULT STDAPICALLTYPE DllRegisterServer(void) {
    return S_OK;
}
HRESULT STDAPICALLTYPE DllUnregisterServer(void) {
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
