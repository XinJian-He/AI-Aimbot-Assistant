#pragma once
#include "makcu_provider.h"
namespace makcu_provider::scheme2 {
inline constexpr Config config{2, "Compatible SDK / negotiated 4M", "makcu-cpp@9874efa96f58e7a768e0110d765f0fa072e7e25e", "ASCII", "connect(port,true): 115200 -> negotiated 4000000"};
inline std::unique_ptr<Provider> create() { return makeCompatible(); }
}
