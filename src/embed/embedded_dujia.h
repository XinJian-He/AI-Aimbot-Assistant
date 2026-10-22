#pragma once
// ============================================================
//  exe 内嵌 封装dll 的加载接口
//  加密资源在内存中解密 -> 内存加载（不落盘），导出按名称查询
// ============================================================
#include <windows.h>

// 解密并内存加载内嵌的 封装dll，成功返回伪 HMODULE；重复调用返回同一句柄
HMODULE DujiaLoad();

// 按名称查询导出函数：优先内嵌内存模块，其次已加载的外部 封装dll.dll
FARPROC DujiaProc(const char* name);

// 卸载内嵌内存模块（不处理外部 LoadLibrary 的句柄）
void DujiaFree();

// 判断句柄是否为内嵌内存模块（是则不应 FreeLibrary，而应 DujiaFree）
bool DujiaIsEmbedded(HMODULE h);
