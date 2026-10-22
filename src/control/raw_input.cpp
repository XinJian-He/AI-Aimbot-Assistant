#include "raw_input.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wtsapi32.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <stdexcept>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "wtsapi32.lib")

namespace RawInput {
namespace {
std::mutex lifecycleMutex;
std::mutex statusMutex;
std::condition_variable started;
std::thread worker;
HANDLE stopEvent = nullptr;
bool startupDone = false;
std::atomic<bool> ready{false};
std::string lastError;
std::array<std::atomic<bool>, 256> keys{};
// Only the input thread modifies these per-device states.
std::unordered_map<HANDLE, std::array<bool, 256>> devices;

void Publish() {
    std::array<bool, 256> merged{};
    for (const auto& device : devices)
        for (size_t i = 0; i < merged.size(); ++i)
            merged[i] = merged[i] || device.second[i];
    merged[VK_SHIFT] = merged[VK_LSHIFT] || merged[VK_RSHIFT];
    merged[VK_CONTROL] = merged[VK_LCONTROL] || merged[VK_RCONTROL];
    merged[VK_MENU] = merged[VK_LMENU] || merged[VK_RMENU];
    for (size_t i = 0; i < keys.size(); ++i) keys[i].store(merged[i]);
}

void Clear() {
    devices.clear();
    Publish();
}

void RemoveDevice(HANDLE device) {
    devices.erase(device);
    Publish();
}

void ApplyInput(const RAWINPUT& input) {
    if (input.header.dwType == RIM_TYPEMOUSE) {
        auto& state = devices[input.header.hDevice];
        const USHORT flags = input.data.mouse.usButtonFlags;
        constexpr int buttons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        constexpr USHORT down[] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN,
            RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
        constexpr USHORT up[] = {RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP,
            RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
        for (size_t i = 0; i < 5; ++i) {
            if (flags & down[i]) state[buttons[i]] = true;
            if (flags & up[i]) state[buttons[i]] = false;
        }
    } else if (input.header.dwType == RIM_TYPEKEYBOARD) {
        const auto& keyboard = input.data.keyboard;
        UINT vk = keyboard.VKey;
        if (!vk || vk >= 255 || keyboard.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE) return;
        if (vk == VK_SHIFT)
            vk = MapVirtualKeyW(keyboard.MakeCode, MAPVK_VSC_TO_VK_EX);
        else if (vk == VK_CONTROL)
            vk = keyboard.Flags & RI_KEY_E0 ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU)
            vk = keyboard.Flags & RI_KEY_E0 ? VK_RMENU : VK_LMENU;
        if (!vk || vk >= keys.size()) return;
        devices[input.header.hDevice][vk] = !(keyboard.Flags & RI_KEY_BREAK);
    } else return;
    Publish();
}

bool DecodeInput(const void* data, size_t size) {
    if (!data || size < sizeof(RAWINPUTHEADER)) return false;
    const auto* input = static_cast<const RAWINPUT*>(data);
    if (input->header.dwSize > size || input->header.dwSize < sizeof(RAWINPUTHEADER)) return false;
    const size_t payload = input->header.dwType == RIM_TYPEMOUSE ? sizeof(RAWMOUSE) :
        input->header.dwType == RIM_TYPEKEYBOARD ? sizeof(RAWKEYBOARD) : 0;
    if (!payload || input->header.dwSize < offsetof(RAWINPUT, data) + payload) return false;
    ApplyInput(*input);
    return true;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INPUT) {
        UINT size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr,
            &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            size >= sizeof(RAWINPUTHEADER) && size <= 65536) {
            // RAWINPUT alignment, including when the incoming payload is larger than RAWINPUT.
            std::vector<RAWINPUT> buffer((size + sizeof(RAWINPUT) - 1) / sizeof(RAWINPUT));
            const UINT capacity = size;
            const UINT got = GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                buffer.data(), &size, sizeof(RAWINPUTHEADER));
            if (got != UINT(-1) && got <= capacity) DecodeInput(buffer.data(), got);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (message == WM_INPUT_DEVICE_CHANGE && wparam == GIDC_REMOVAL) {
        RemoveDevice(reinterpret_cast<HANDLE>(lparam));
        return 0;
    }
    if (message == WM_WTSSESSION_CHANGE &&
        (wparam == WTS_SESSION_LOCK || wparam == WTS_CONSOLE_DISCONNECT ||
         wparam == WTS_REMOTE_DISCONNECT || wparam == WTS_SESSION_LOGOFF)) {
        Clear();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void InputLoop() {
    const wchar_t* className = L"AimRawInputWindow";
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND hwnd = nullptr;
    bool registered = false;
    bool inputsRegistered = false;
    bool sessionRegistered = false;
    try {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance;
        wc.lpfnWndProc = WindowProc;
        wc.lpszClassName = className;
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) throw std::runtime_error("Raw Input 窗口类注册失败");
        hwnd = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, instance, nullptr);
        if (!hwnd) throw std::runtime_error("Raw Input 窗口创建失败");
        RAWINPUTDEVICE rid[] = {
            {0x01, 0x06, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd},
            {0x01, 0x02, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd}
        };
        if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)))
            throw std::runtime_error("Raw Input 键盘/鼠标注册失败");
        inputsRegistered = true;
        sessionRegistered = WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION) != FALSE;
        if (!sessionRegistered) throw std::runtime_error("Raw Input 会话通知注册失败");
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            ready.store(true);
            startupDone = true;
        }
        started.notify_all();
        bool done = false;
        while (!done) {
            const DWORD result = MsgWaitForMultipleObjectsEx(1, &stopEvent, INFINITE,
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_OBJECT_0) break;
            if (result == WAIT_FAILED) throw std::runtime_error("Raw Input 消息等待失败");
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT || WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                    done = true;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(statusMutex);
        lastError = e.what();
    } catch (...) {
        std::lock_guard<std::mutex> lock(statusMutex);
        lastError = "Raw Input 线程发生未知异常";
    }
    ready.store(false);
    if (sessionRegistered) WTSUnRegisterSessionNotification(hwnd);
    if (inputsRegistered) {
        RAWINPUTDEVICE remove[] = {{0x01, 0x06, RIDEV_REMOVE, nullptr}, {0x01, 0x02, RIDEV_REMOVE, nullptr}};
        RegisterRawInputDevices(remove, 2, sizeof(RAWINPUTDEVICE));
    }
    if (hwnd) DestroyWindow(hwnd);
    if (registered) UnregisterClassW(className, instance);
    Clear();
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        startupDone = true;
    }
    started.notify_all();
}
}

bool Start() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    if (ready.load()) return true;
    if (worker.joinable()) worker.join();
    if (stopEvent) CloseHandle(stopEvent);
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        startupDone = false;
        lastError.clear();
        if (!stopEvent) {
            lastError = "Raw Input 停止事件创建失败";
            return false;
        }
    }
    Clear();
    try {
        worker = std::thread(InputLoop);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(statusMutex);
        lastError = e.what();
        CloseHandle(stopEvent);
        stopEvent = nullptr;
        return false;
    }
    std::unique_lock<std::mutex> lock(statusMutex);
    started.wait(lock, [] { return startupDone; });
    return ready.load();
}

void Stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    if (stopEvent) SetEvent(stopEvent);
    if (worker.joinable()) worker.join();
    if (stopEvent) CloseHandle(stopEvent);
    stopEvent = nullptr;
    ready.store(false);
    Clear();
}

bool IsReady() { return ready.load(); }
bool IsPressed(int vk) {
    return ready.load() && vk > 0 && vk < static_cast<int>(keys.size()) && keys[vk].load();
}
std::string GetLastError() {
    std::lock_guard<std::mutex> lock(statusMutex);
    return lastError;
}
}
