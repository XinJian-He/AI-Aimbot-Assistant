#pragma once
#include "makcu_provider.h"
namespace makcu_provider::scheme4 {
inline constexpr Config config{4, "Win32 ASCII (legacy)", "builtin-win32-ascii-v1", "ASCII", "Explicit COM: configured baud; AUTO: 115200 / 4000000"};
std::unique_ptr<Provider> create();
}
