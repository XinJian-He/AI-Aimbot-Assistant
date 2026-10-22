#pragma once
#include <string>

namespace RawInput {
bool Start();
void Stop();
bool IsReady();
bool IsPressed(int vk);
std::string GetLastError();
}
