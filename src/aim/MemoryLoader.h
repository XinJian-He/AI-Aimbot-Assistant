#pragma once
// ============================================================
//  最小化 PE 内存加载器（x64）
//  支持: 区段映射 / 重定位 / 导入表 / 导出查询 / 入口点调用 / 异常处理表(pdata)注册
//  不支持: 延迟导入 / TLS 回调
//  用于加载 封装dll_unpacked.dll 与 NeuralCurveModule_unpacked.dll
// ============================================================
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// 在内存中加载一个完整 PE 映像，返回可传给 MemGetProcAddress 的伪 HMODULE
HMODULE MemLoadLibrary(const void* image, SIZE_T size);

// 按名称查询导出函数地址
FARPROC MemGetProcAddress(HMODULE hModule, const char* name);

// 触发 DLL_PROCESS_DETACH 并释放映像内存
void MemFreeLibrary(HMODULE hModule);

#ifdef __cplusplus
}
#endif
