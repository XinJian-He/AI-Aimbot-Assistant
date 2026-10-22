#pragma once
// ============================================================
//  exe 内嵌 NeuralCurveModule 的加载接口
//  加密资源在内存中解密 -> 内存加载（不落盘），导出按名称查询
//  与 embedded_dujia 同模式；独立密钥种子(0x51F57A93)与数组名(kNNEnc)
// ============================================================
#include <windows.h>

// 解密并内存加载内嵌的 NeuralCurveModule，成功返回伪 HMODULE；重复调用返回同一句柄
HMODULE NnLoad();

// 按名称查询导出函数：优先内嵌内存模块，其次已加载的外部 NeuralCurveModule.dll
FARPROC NnProc(const char* name);

// 卸载内嵌内存模块（不处理外部 LoadLibrary 的句柄）
void NnFree();

// 判断句柄是否为内嵌内存模块（是则不应 FreeLibrary，而应 NnFree）
bool NnIsEmbedded(HMODULE h);
