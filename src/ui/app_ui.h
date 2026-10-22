#pragma once
#include <cstdint>
#include <windows.h>

namespace AppUI
{
void Initialize(float scale, bool light);
void SetHumanTexture(std::uint64_t texture);
void SetAvatarTexture(std::uint64_t texture);
bool LayoutFits();
void Render(HWND window);
bool CanClose(HWND window);
void Shutdown();
void Tick(bool visible = true);
} // namespace AppUI
