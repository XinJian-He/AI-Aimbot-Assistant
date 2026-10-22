#include "../common/obfuscated_text.h"
// Dedicated native UI thread. Backend ownership remains with the Aim host.
#define NOMINMAX
#include "app_ui.h"
#include "ui_backend.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <future>
#include <stdexcept>
#include "image_texture.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <algorithm>
#include <cmath>
#include <crtdbg.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <windowsx.h>
static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
static IDXGISwapChain1 *g_pSwapChain = nullptr;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static float g_uiScale = 1;
// Wall-clock pacing is independent of the monitor refresh rate. Input wakes
// the message wait immediately, then renders on the next interactive deadline.
constexpr ULONGLONG kInteractiveFrameMs = 8;  // About 120 FPS (125 FPS timer cap; VSync still applies)
constexpr ULONGLONG kIdleFrameMs = 200;       // 5 FPS for visible telemetry
constexpr ULONGLONG kBackgroundFrameMs = 500; // 2 FPS when unfocused
constexpr ULONGLONG kHiddenTickMs = 250;      // Backend completions only
static ULONGLONG g_lastInteraction = 0;
static void WaitForUiMessages(ULONGLONG milliseconds)
{
    MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(milliseconds),
                               QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}
constexpr float kShadowMargin = 24.f;
static Microsoft::WRL::ComPtr<IDCompositionDevice> g_composition;
static Microsoft::WRL::ComPtr<IDCompositionTarget> g_compositionTarget;
static Microsoft::WRL::ComPtr<IDCompositionVisual> g_visual;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_humanTexture;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_avatarTexture;
bool CreateDeviceD3D(HWND);
void CleanupDeviceD3D();
bool CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
thread_local ImGuiContext* AimImGuiContext = nullptr;
extern "C" IMAGE_DOS_HEADER __ImageBase;
static std::atomic<bool> g_stopUI{false}, g_uiOpen{false};
static std::atomic<HWND> g_uiWindow{nullptr};
struct UILifecycle { std::mutex mutex; std::thread worker; };
static UILifecycle& Lifecycle() { static auto* state=new UILifecycle; return *state; }

static void RunNativeWindow(std::promise<int>& ready)
{
    HINSTANCE instance=reinterpret_cast<HINSTANCE>(&__ImageBase);
    HWND hwnd=nullptr;
    bool signaled=false, win32=false, dx11=false, context=false, registered=false;
    HRESULT comResult=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    // Thread-local DPI awareness avoids modifying the application's other windows.
    auto oldDpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const wchar_t* className=AIM_TEXT(L"AimNativeImGuiWindow");
    try {
        if(FAILED(comResult)) throw std::runtime_error(AIM_TEXT("COM initialization failed"));
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&monitor);
        int mw=monitor.rcWork.right-monitor.rcWork.left, mh=monitor.rcWork.bottom-monitor.rcWork.top;
        int width=std::max(400,std::min(1100,mw-64));
        int height=std::max(300,std::min(744,mh-64));
        g_uiScale=std::min(width/1020.f,height/690.f);
        width+=int(kShadowMargin*2);height+=int(kShadowMargin*2);
        WNDCLASSEXW wc{sizeof(wc),CS_HREDRAW|CS_VREDRAW,WndProc,0,0,instance,nullptr,
            LoadCursor(nullptr,IDC_ARROW),nullptr,nullptr,className,nullptr};
        registered=RegisterClassExW(&wc)!=0;
        if(!registered)throw std::runtime_error(AIM_TEXT("Window class registration failed"));
        hwnd=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_NOREDIRECTIONBITMAP,className,AIM_TEXT(L"VISION"),
            WS_POPUP|WS_MINIMIZEBOX|WS_MAXIMIZEBOX,monitor.rcWork.left+(mw-width)/2,
            monitor.rcWork.top+(mh-height)/2,width,height,nullptr,nullptr,instance,nullptr);
        if(!hwnd||!CreateDeviceD3D(hwnd))throw std::runtime_error(AIM_TEXT("D3D11 window initialization failed"));
        BOOL dark=TRUE;DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
        int round=1;DwmSetWindowAttribute(hwnd,33,&round,sizeof(round));
        DWMNCRENDERINGPOLICY nonclient=DWMNCRP_DISABLED;
        DwmSetWindowAttribute(hwnd,DWMWA_NCRENDERING_POLICY,&nonclient,sizeof(nonclient));
        IMGUI_CHECKVERSION();ImGui::CreateContext();context=true;
        AppUI::Initialize(g_uiScale,false);
        if(SUCCEEDED(LoadHumanTexture(g_pd3dDevice,&g_humanTexture,instance)))
            AppUI::SetHumanTexture(reinterpret_cast<std::uint64_t>(g_humanTexture.Get()));
        if(SUCCEEDED(LoadHumanTexture(g_pd3dDevice,&g_avatarTexture,instance,102)))
            AppUI::SetAvatarTexture(reinterpret_cast<std::uint64_t>(g_avatarTexture.Get()));
        win32=ImGui_ImplWin32_Init(hwnd);
        if(!win32)throw std::runtime_error(AIM_TEXT("Win32 backend initialization failed"));
        ImGui_ImplWin32_SetClientOrigin(kShadowMargin,kShadowMargin);
        dx11=ImGui_ImplDX11_Init(g_pd3dDevice,g_pd3dDeviceContext);
        if(!dx11)throw std::runtime_error(AIM_TEXT("ImGui renderer initialization failed"));
        g_uiWindow=hwnd;g_uiOpen=true;
        ShowWindow(hwnd,SW_SHOW);UpdateWindow(hwnd);
        ready.set_value(1);signaled=true;
        ULONGLONG lastFrame = 0, lastHiddenTick = 0;
        bool occluded = false;
        g_lastInteraction = GetTickCount64();
        while(!g_stopUI) {
            MSG msg;
            while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
                if(msg.message==WM_QUIT){g_stopUI=true;break;}
                TranslateMessage(&msg);DispatchMessageW(&msg);
            }
            if(g_stopUI)break;
            const auto now = GetTickCount64();
            if(IsIconic(hwnd) || !IsWindowVisible(hwnd)) {
                if(now-lastHiddenTick >= kHiddenTickMs) {
                    AppUI::Tick(false);
                    lastHiddenTick=now;
                }
                WaitForUiMessages(kHiddenTickMs-(now-lastHiddenTick));
                continue;
            }
            const bool foreground = GetForegroundWindow()==hwnd;
            const auto interval = occluded ? kBackgroundFrameMs :
                !foreground ? kBackgroundFrameMs :
                now-g_lastInteraction < 600 ? kInteractiveFrameMs : kIdleFrameMs;
            if(now-lastFrame < interval) {
                WaitForUiMessages(interval-(now-lastFrame));
                continue;
            }
            lastFrame=now;
            if(occluded) {
                // Probe visibility without submitting another rendered frame.
                const HRESULT visible=g_pSwapChain->Present(0,DXGI_PRESENT_TEST);
                if(visible==DXGI_STATUS_OCCLUDED) { AppUI::Tick(false); continue; }
                if(FAILED(visible))throw std::runtime_error(AIM_TEXT("D3D11 visibility test failed"));
                occluded=false;
            }
            if(g_ResizeWidth&&g_ResizeHeight){
                CleanupRenderTarget();g_pd3dDeviceContext->OMSetRenderTargets(0,nullptr,nullptr);
                HRESULT hr=g_pSwapChain->ResizeBuffers(0,g_ResizeWidth,g_ResizeHeight,DXGI_FORMAT_UNKNOWN,0);
                g_ResizeWidth=g_ResizeHeight=0;
                if(FAILED(hr)||!CreateRenderTarget())throw std::runtime_error(AIM_TEXT("D3D11 resize failed"));
            }
            ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();
            auto& io=ImGui::GetIO();io.DisplaySize.x-=2*kShadowMargin;io.DisplaySize.y-=2*kShadowMargin;
            ImGui::NewFrame();AppUI::Render(hwnd);ImGui::Render();
            if(ImGui::IsAnyItemActive() || io.WantTextInput)
                g_lastInteraction=GetTickCount64();
            auto* draw=ImGui::GetDrawData();draw->DisplayPos={-kShadowMargin,-kShadowMargin};
            draw->DisplaySize.x+=2*kShadowMargin;draw->DisplaySize.y+=2*kShadowMargin;
            const float bg[]={0,0,0,0};g_pd3dDeviceContext->OMSetRenderTargets(1,&g_mainRenderTargetView,nullptr);
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,bg);
            ImGui_ImplDX11_RenderDrawData(draw);
            HRESULT hr=g_pSwapChain->Present(1,0);
            if(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET)throw std::runtime_error(AIM_TEXT("D3D11 device lost"));
            occluded=hr==DXGI_STATUS_OCCLUDED;
        }
    } catch(const std::exception& e) {OutputDebugStringA(e.what());}
      catch(...) {OutputDebugStringA(AIM_TEXT("Native UI failed"));}
    // Finish all API operations before allowing the backend/module to shut down.
    if(context)AppUI::Shutdown();
    g_uiWindow=nullptr;
    if(dx11)ImGui_ImplDX11_Shutdown();
    if(win32)ImGui_ImplWin32_Shutdown();
    if(context)ImGui::DestroyContext();
    CleanupDeviceD3D();
    if(hwnd&&IsWindow(hwnd))DestroyWindow(hwnd);
    if(registered)UnregisterClassW(className,instance);
    g_ResizeWidth=g_ResizeHeight=0;
    if(oldDpi)SetThreadDpiAwarenessContext(oldDpi);
    if(SUCCEEDED(comResult))CoUninitialize();
    g_uiOpen=false;
    if(!signaled)ready.set_value(0);
}
extern "C" AIM_API int AIM_CALL Aim_ShowUI(void) {
    try {
        auto& life=Lifecycle();std::lock_guard<std::mutex> lock(life.mutex);
        if(g_uiOpen){if(auto hwnd=g_uiWindow.load()){ShowWindowAsync(hwnd,SW_RESTORE);SetForegroundWindow(hwnd);}return 1;}
        if(life.worker.joinable())life.worker.join();
        g_stopUI=false;
        std::promise<int> ready;auto result=ready.get_future();
        life.worker=std::thread([ready=std::move(ready)]() mutable {RunNativeWindow(ready);});
        return result.get();
    }catch(...){return 0;}
}
extern "C" AIM_API void AIM_CALL Aim_CloseUI(void) {
    auto& life=Lifecycle();std::lock_guard<std::mutex> lock(life.mutex);
    g_stopUI=true;
    if(auto hwnd=g_uiWindow.load())PostMessageW(hwnd,WM_NULL,0,0);
    if(life.worker.joinable()&&life.worker.get_id()!=std::this_thread::get_id())life.worker.join();
}
extern "C" AIM_API int AIM_CALL Aim_IsUIOpen(void) {return g_uiOpen?1:0;}

// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL feature{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   2, D3D11_SDK_VERSION, &g_pd3dDevice, &feature, &g_pd3dDeviceContext);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                               D3D11_SDK_VERSION, &g_pd3dDevice, &feature, &g_pd3dDeviceContext);
    if (FAILED(hr))
        return false;
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIDevice> device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&device))) || FAILED(device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;
    RECT client{};
    GetClientRect(hWnd, &client);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = client.right;
    desc.Height = client.bottom;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(factory->CreateSwapChainForComposition(g_pd3dDevice, &desc, nullptr, &g_pSwapChain)))
        return false;
    if (FAILED(DCompositionCreateDevice(device.Get(), IID_PPV_ARGS(&g_composition))) ||
        FAILED(g_composition->CreateTargetForHwnd(hWnd, TRUE, &g_compositionTarget)) ||
        FAILED(g_composition->CreateVisual(&g_visual)) || FAILED(g_visual->SetContent(g_pSwapChain)) ||
        FAILED(g_compositionTarget->SetRoot(g_visual.Get())) || FAILED(g_composition->Commit()))
        return false;
    return CreateRenderTarget();
}
void CleanupDeviceD3D()
{
    g_humanTexture.Reset();
    g_avatarTexture.Reset();
    g_visual.Reset();
    g_compositionTarget.Reset();
    g_composition.Reset();
    CleanupRenderTarget();
    if (g_pSwapChain)
    {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext)
    {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice)
    {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

bool CreateRenderTarget()
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        return false;
    return SUCCEEDED(g_pd3dDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_mainRenderTargetView));
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView)
    {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite
// your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or
// clear/overwrite your copy of the keyboard data. Generally you may always pass all inputs to dear imgui, and hide them
// from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if((msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST) ||
       (msg>=WM_KEYFIRST && msg<=WM_KEYLAST) || msg==WM_MOUSELEAVE ||
       msg==WM_SETFOCUS || msg==WM_ACTIVATE || msg==WM_SIZE || msg==WM_PAINT)
        g_lastInteraction=GetTickCount64();
    if (msg == WM_NCCALCSIZE && wParam)
        return 0;
    if (msg == WM_GETMINMAXINFO)
    {
        auto info = reinterpret_cast<MINMAXINFO *>(lParam);
        info->ptMinTrackSize = {960 + static_cast<LONG>(2 * kShadowMargin), 620 + static_cast<LONG>(2 * kShadowMargin)};
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            info->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left,
                                   monitor.rcWork.top - monitor.rcMonitor.top};
            info->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top};
        }
        return 0;
    }
    if (msg == WM_NCHITTEST)
    {
        POINT mouse{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hWnd, &mouse);
        RECT r{};
        GetClientRect(hWnd, &r);
        mouse.x -= static_cast<LONG>(kShadowMargin);
        mouse.y -= static_cast<LONG>(kShadowMargin);
        r.right -= static_cast<LONG>(2 * kShadowMargin);
        r.bottom -= static_cast<LONG>(2 * kShadowMargin);
        // The transparent rounded corners must not intercept desktop clicks.
        float actualScale = std::min(r.right / 1020.f, r.bottom / 690.f);
        float radius = 32 * actualScale;
        float cx = std::clamp(static_cast<float>(mouse.x), radius, r.right - radius);
        float cy = std::clamp(static_cast<float>(mouse.y), radius, r.bottom - radius);
        if ((mouse.x - cx) * (mouse.x - cx) + (mouse.y - cy) * (mouse.y - cy) > radius * radius)
            return HTTRANSPARENT;
        int edge = 6;
        bool left = mouse.x < edge, right = mouse.x >= r.right - edge;
        bool top = mouse.y < edge, bottom = mouse.y >= r.bottom - edge;
        if (top && left)
            return HTTOPLEFT;
        if (top && right)
            return HTTOPRIGHT;
        if (bottom && left)
            return HTBOTTOMLEFT;
        if (bottom && right)
            return HTBOTTOMRIGHT;
        if (left)
            return HTLEFT;
        if (right)
            return HTRIGHT;
        if (top)
            return HTTOP;
        if (bottom)
            return HTBOTTOM;
        if (mouse.y < 78 * actualScale && mouse.x < r.right - 140 * actualScale)
            return HTCAPTION;
        return HTCLIENT;
    }
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_CLOSE:
        if (AppUI::CanClose(hWnd)) g_stopUI=true;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED: {
        auto rect = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(hWnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
