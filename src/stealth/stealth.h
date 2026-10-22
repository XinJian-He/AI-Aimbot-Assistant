#pragma once
// ============================================================
//  stealth.h — 隐蔽模块技术（借鉴 4.0detaQQLive 方案）
//  PEB 模块链表 unlink：把模块从 PEB 的 3 条 LDR 链表摘除，
//  使模块对 EnumProcessModules / Module32First(ToolHelp) 等
//  用户态模块枚举不可见，但代码/数据仍在内存中正常运行。
// ============================================================
#include <Windows.h>

namespace Stealth {

// 从 PEB 3 条 LDR 链表摘除指定模块 + 清空名字。nullptr=主程序 exe
void unlinkModule(HMODULE hModule);

// 按 DLL 名（大小写不敏感、部分匹配）摘除
void unlinkModuleByName(const wchar_t* dllName);

// 擦除模块 PE 头（前 0x1000 字节），防内存扫描按 MZ/PE 识别
void erasePEHeader(HMODULE hModule);

}
