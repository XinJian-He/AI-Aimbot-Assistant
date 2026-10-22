#include "pid.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <utility>
#include <wchar.h>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include "kmboxNet.h"
#include "makcu.h"
#include "embedded_dujia.h"
#include "embedded_nn.h"
extern std::atomic<bool> g_kmConnected;   // 定义在 AimBackend.cpp（KMBox 连接状态）

// main.cpp 中的日志函数，将消息写入 ImGui 控制台
extern void PushLog(const std::string& s);

// ============================================================
//  P_PID —— 来自 pid1.cpp 的算法（5参数：比例/微分/预测/速率/平滑）
// ============================================================
namespace {

constexpr double PID_BANDWIDTH = 10000.0;
constexpr double SOFT_LIMIT_NUMERATOR = 4.0 / 15.0;
constexpr double SOFT_LIMIT_DENOMINATOR = 3.0 / 5.0;

double smoothTerm(double value, double bandwidth, double outputScale) {
    double ratio = value / bandwidth;
    double squared = ratio * ratio;
    return (ratio * (1.0 + SOFT_LIMIT_NUMERATOR * squared) /
        (1.0 + SOFT_LIMIT_DENOMINATOR * squared)) * outputScale;
}

} // namespace

class P_PID {
public:
    P_PID() = default;

    bool init(double kp_, double kd_, double predict_, double rate_, double smooth_) {
        kp = kp_;
        kd = kd_;
        bandwidth = PID_BANDWIDTH;
        smooth = smooth_;
        kp_gain_rate = rate_;
        predict = predict_;
        ki_deadband = 0.5;
        return true;
    }

    double update(double error) {
        if (std::abs(error) < 0.3) error = 0.0;
        if (std::abs(error - last_error) > 30.0) reset();

        adjust_integral(error);
        kp_integral(error);

        double error_diff = error - last_error;
        double target_velocity = error_diff + last_u;
        target_velocity = update_velocity_filter(target_velocity);

        double raw_velocity_input = target_velocity;
        if (std::abs(error) < 1.0 && std::abs(error_diff) < 0.1) {
            raw_velocity_input = error_diff + last_u * 0.5;
        }

        double ki_raw = raw_velocity_input;
        ki_raw = (std::abs(ki_raw) > ki_deadband) ? ki_raw : 0.0;
        ki_raw = (ki_raw * predict) * integral_gain;
        ki_raw = update_integral_filter(ki_raw);

        double K_p = kp * error;
        double K_i = ki_raw;
        double K_d = kd * (error - last_error);

        if (smooth) {
            K_p = smoothTerm(K_p, bandwidth, bandwidth - smooth);
            K_i = smoothTerm(K_i, bandwidth, bandwidth - 1000.0);
            K_d = smoothTerm(K_d, bandwidth, bandwidth - smooth);
        }

        double u = K_p + K_i + K_d;
        u_filtered = u * kp_gain;
        last_u = u_filtered;
        last_error = error;
        last_integral_term = integral_term;
        return u_filtered;
    }

    void reset() {
        kp_gain = 0.0;
        integral_gain = 0.0;
        u_filtered = 0.0;
        last_error = 0.0;
        integral_term = 0.0;
        last_u = 0.0;
        velocity_filter_x = 0.0;
        velocity_filter_p = 0.0;
        integral_filter_x = 0.0;
        integral_filter_p = 0.0;
    }

private:
    void adjust_integral(double error) {
        double abs_error = std::abs(error);
        if (abs_error < integral_gain_threshold) {
            double ratio = 1.0 - (abs_error / integral_gain_threshold);
            integral_gain += (ratio - integral_gain) * integral_gain_rate;
        } else {
            double ratio = integral_gain_threshold / abs_error;
            integral_gain += (ratio * integral_gain - integral_gain) * 0.1;
        }
        integral_gain = std::clamp(integral_gain, 0.0, 1.0);
    }

    void kp_integral(double error) {
        double abs_error = std::abs(error);
        if (abs_error < kp_gain_threshold) {
            double ratio = 1.0 - (abs_error / kp_gain_threshold);
            kp_gain += (ratio - kp_gain) * kp_gain_rate;
        } else {
            double ratio = kp_gain_threshold / abs_error;
            kp_gain += (ratio * kp_gain - kp_gain) * 0.1;
        }
        kp_gain = std::clamp(kp_gain, 0.0, 1.0);
    }

    double update_velocity_filter(double measurement) {
        constexpr double q = 0.01;
        constexpr double r = 1.0;
        double predicted_x = velocity_filter_x;
        double predicted_p = velocity_filter_p + q;
        double k = predicted_p / (predicted_p + r);
        velocity_filter_x = predicted_x + k * (measurement - predicted_x);
        velocity_filter_p = (1 - k) * predicted_p;
        return velocity_filter_x;
    }

    double update_integral_filter(double measurement) {
        constexpr double q = 0.5;
        constexpr double r = 1.0;
        double predicted_x = integral_filter_x;
        double predicted_p = integral_filter_p + q;
        double k = predicted_p / (predicted_p + r);
        integral_filter_x = predicted_x + k * (measurement - predicted_x);
        integral_filter_p = (1 - k) * predicted_p;
        return integral_filter_x;
    }

    double kp = 0.0;
    double kd = 0.0;
    double bandwidth = 0.0;
    double smooth = 0.0;
    double predict = 0.0;
    double ki_deadband = 0.0;

    double u_filtered = 0.0;
    double integral_term = 0.0;
    double last_integral_term = 0.0;
    double last_error = 0.0;
    double last_u = 0.0;

    double kp_gain = 0.0;
    double kp_gain_threshold = 1920.0;
    double kp_gain_rate = 0.03;

    double integral_gain = 0.0;
    double integral_gain_threshold = 50.0;
    double integral_gain_rate = 0.025;

    double velocity_filter_x = 0.0;
    double velocity_filter_p = 1.0;
    double integral_filter_x = 0.0;
    double integral_filter_p = 1.0;
};

// ============================================================
//  MoveMethod
// ============================================================
std::atomic<MoveMethod> g_moveMethod{ MoveMethod::KMBOX };
std::atomic<int> g_outputDevice{ -1 };

// ============================================================
//  MAKCU 驱动（串口盒子）
// ============================================================
bool InitMAKCUDriver(const char* portName, int baudRate) {
    if (makcu_is_connected()) return true;
    return makcu_init(portName, baudRate);
}

void ShutdownMAKCUDriver() {
    makcu_close();
}

// ============================================================
//  独家驱动 (封装dll.dll, LoadLibrary-based)
//  one()                成功加载返回 TRUE
//  tow()                填充 MOUSE_INPUT_DATA 后调用移动
//  three(pid, bHide)    隐藏/显示指定 PID 进程（Home 键切换自身）
// ============================================================
typedef struct _MOUSE_INPUT_DATA {
    USHORT UnitId;
    USHORT Flags;
    union {
        ULONG Buttons;
        struct {
            USHORT ButtonFlags;
            USHORT ButtonData;
        };
    };
    ULONG  RawButtons;
    LONG   LastX;
    LONG   LastY;
    ULONG  ExtraInformation;
} MOUSE_INPUT_DATA;

// 标志位与 Windows kbdmou.h 一致（独家驱动为内核驱动，按内核语义解析 Flags）
enum move_type {
    MOVE_RELATIVE = 0x01,  // MOUSE_MOVE_RELATIVE  相对移动
    MOVE_ABSOLUTE = 0x02,  // MOUSE_MOVE_ABSOLUTE  绝对移动
};
enum button_flags {
    MOVE_NULL = 0,               // 不进行按下抬起 只移动
    LEFT_BUTTON_DOWN = 0x0001,   // 左键按下
    LEFT_BUTTON_UP = 0x0002,     // 左键抬起
    RIGHT_BUTTON_DOWN = 0x0004,  // 右键按下
    RIGHT_BUTTON_UP = 0x0008,    // 右键抬起
    MIDDLE_BUTTON_DOWN = 0x0010, // 中键按下
    MIDDLE_BUTTON_UP = 0x0020,   // 中键抬起
};

typedef BOOL(__cdecl *FnDujiaOne)();
typedef BOOL(__cdecl *FnDujiaTow)(MOUSE_INPUT_DATA*);
typedef BOOL(__cdecl *FnDujiaThree)(ULONG pid, BOOL bHide);
static HMODULE  g_dujiaDll = nullptr;
static FnDujiaOne g_dujia_one = nullptr;
static FnDujiaTow  g_dujia_tow = nullptr;
static FnDujiaThree g_dujia_three = nullptr;
static ULONG    g_hiddenPid = 0;    // 当前已隐藏的 PID，0 = 未隐藏
static std::atomic<bool> g_dujiaLoaded{ false };

bool InitDujiaDriver(std::string* error) {
    if (g_dujiaLoaded) return true;
    // 优先使用 exe 内嵌的 封装dll（加密资源 + 内存加载，不落盘），目录文件兜底
    g_dujiaDll = DujiaLoad();
    if (!g_dujiaDll) g_dujiaDll = LoadLibraryW(L"封装dll.dll");
    if (!g_dujiaDll) {
        if (error) *error = "封装dll 加载失败（内嵌资源与本地文件均不可用），Windows 错误码: " + std::to_string(GetLastError());
        PushLog("[独家] 封装dll 加载失败（内嵌资源与本地文件均不可用）");
        return false;
    }
    g_dujia_one = (FnDujiaOne)DujiaProc("one");
    g_dujia_tow = (FnDujiaTow)DujiaProc("tow");
    g_dujia_three = (FnDujiaThree)DujiaProc("three");
    if (!g_dujia_one || !g_dujia_tow || !g_dujia_three) {
        if (error) *error = "无法解析驱动导出函数(one/tow/three)";
        PushLog("[独家] 无法解析导出函数(one/tow/three)");
        ShutdownDujiaDriver();
        return false;
    }
    if (!g_dujia_one()) {
        if (error) *error = "驱动初始化失败（one()=FALSE），请确认驱动已安装并以管理员运行";
        PushLog("[独家] 驱动未加载（one()=FALSE：请管理员运行 驱动.exe 安装驱动并重启，再以管理员运行本程序）");
        ShutdownDujiaDriver();
        return false;
    }
    g_dujiaLoaded = true;
    PushLog("[独家] 独家驱动已加载");
    return true;
}

void ShutdownDujiaDriver() {
    if (g_dujiaDll) {
        if (DujiaIsEmbedded(g_dujiaDll)) DujiaFree();  // 内嵌内存模块
        else FreeLibrary(g_dujiaDll);                  // 外部文件
        g_dujiaDll = nullptr;
    }
    g_dujia_one = nullptr;
    g_dujia_tow = nullptr;
    g_dujia_three = nullptr;
    g_hiddenPid = 0;
    g_dujiaLoaded = false;
}

// 运行模式（proxy.cpp 定义并写入）：0=注入宿主模式(如 QQLive.exe) 1=独立模式
// selectany 弱定义兜底：ui-design.exe 等不含 proxy.cpp 的构建单独链接此符号
extern "C" __declspec(selectany) int g_aimRunMode = 0;

// ============================================================
//  独家驱动：Home 键切换隐藏 aim
//  注入宿主模式(g_aimRunMode==0，QQLive 劫持)：aim 后端只跑在
//    主实例进程（QQLive 主进程，互斥体首个获得者，proxy.cpp）里，
//    若 three() 隐藏当前进程 = 隐藏宿主主进程 = 整个应用消失。
//    因此本模式绝不隐藏当前进程，只驱动级隐藏宿主家族的
//    一个后台子进程（排除主进程与当前进程，取内存占用最高者，
//    即渲染游戏/直播画面的那个），其余进程不动。
//  只隐藏进程，不碰任何 UI 窗口（网页控制台、OpenCV 预览保持原样）：
//    曾把 UI 切换接到 Aim_OpenBrowser（Home 按一次弹一次浏览器），
//    浏览器抢前台焦点盖住游戏画面，DXGI 中心区域截图截到浏览器/
//    桌面/文件管理器（"截图穿过游戏窗口"问题），已移除。
//  找不到可隐藏的后台子进程（或驱动不可用）时仅记录隐藏状态，
//    不兜底、不枚举乱隐藏。
//  独立模式：驱动 three(pid) VAD unlink 隐藏自身进程（任务管理器不可见）
//  已知风险：独家驱动对进程做隐藏(VAD unlink)时存在坏指针解引用
//  （映像偏移 0x0a69，悬垂 EPROCESS/竞态）→ 概率性 0x3B 蓝屏。
//  驱动源码在开发机 D:\DeskFile\MyDriver1（本机无），用户态防御：
//  Home 切换加 500ms 冷却 —— 注册表轮询是异步有损握手，快速双击会丢命令，
//  用户态隐藏状态与驱动失配 → 驱动对已隐藏进程重复 unlink → 崩溃。
// ============================================================

// 在宿主进程家族中挑一个"后台子进程"（Home 隐藏目标）：
// 同 exe 名的家族进程里，排除内存占用最高的主进程与当前进程自身，
// 取工作集占用最高的一个（最可能是渲染游戏/直播画面的子进程）。
// 返回候选 PID；没有可用的返回 0。
static ULONG FindBackgroundChild(ULONG curPid) {
    // 宿主 exe 名（如 QQLive.exe）：只在该进程家族内找，不动其他程序
    wchar_t hostExe[64] = {};
    {
        wchar_t host[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, host, MAX_PATH);
        wchar_t* s = wcsrchr(host, L'\\');
        wcsncpy_s(hostExe, s ? s + 1 : host, 63);
    }
    // 收集宿主家族所有进程（PID + 工作集内存）
    std::vector<std::pair<DWORD, DWORD>> procs;   // <pid, workingSetKB>
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, hostExe) != 0) continue;
                DWORD ws = 0;
                HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                       FALSE, pe.th32ProcessID);
                if (h) {
                    PROCESS_MEMORY_COUNTERS pmc{};
                    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
                        ws = (DWORD)(pmc.WorkingSetSize / 1024);
                    CloseHandle(h);
                }
                procs.emplace_back(pe.th32ProcessID, ws);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (procs.empty()) return 0;
    // 主进程 = 工作集最高者（保留不隐藏，避免隐藏整个宿主应用）
    DWORD mainPid = 0, maxWs = 0;
    for (const auto& p : procs)
        if (p.second > maxWs) { maxWs = p.second; mainPid = p.first; }
    // 其余取工作集最高者为隐藏目标（当前进程自身也排除）
    DWORD best = 0, bestWs = 0;
    for (const auto& p : procs) {
        if (p.first == mainPid || p.first == curPid) continue;
        if (p.second > bestWs) { bestWs = p.second; best = p.first; }
    }
    return best;
}


void ToggleDujiaHideSelf() {
    static ULONGLONG s_lastToggleMs = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastToggleMs < 500) return;   // 冷却：防双击 Home 竞态
    s_lastToggleMs = now;

    // 注入宿主模式：aim 后端在主实例进程（QQLive 主进程）里运行，
    // three() 隐藏当前进程 = 隐藏宿主主进程 = 整个应用消失，因此
    // 本模式绝不隐藏当前进程。改为在宿主家族中挑一个后台子进程
    // （排除主进程与当前进程，取工作集最高的那个）做驱动级隐藏，
    // 其余进程不动 → 隐藏目标单一、无并发 VAD unlink 竞态。
    // 找不到后台子进程（或驱动不可用/隐藏失败）时仅记录隐藏状态，
    // 不碰任何 UI 窗口。
    if (g_aimRunMode == 0) {
        static bool s_hidden = false;
        static ULONG s_hiddenPid = 0;    // 已 three() 隐藏的后台子进程 PID（0=仅窗口）
        static bool s_dujiaTried = false; // 驱动加载只尝试一次

        if (s_hidden) {
            // 恢复：解除已隐藏后台子进程的隐藏（UI 不参与切换）
            ULONG hidPid = s_hiddenPid;
            if (g_dujiaLoaded && g_dujia_three && hidPid)
                g_dujia_three(hidPid, FALSE);
            s_hiddenPid = 0;
            s_hidden = false;
            char buf[128];
            if (hidPid)
                snprintf(buf, sizeof(buf), "[独家] 已恢复 aim（后台子进程 pid=%lu 已解除隐藏）", hidPid);
            else
                snprintf(buf, sizeof(buf), "[独家] 已恢复 aim（此前无驱动级隐藏，仅状态复位）");
            PushLog(buf);
        } else {
            if (!g_dujiaLoaded) {
                if (!s_dujiaTried) { s_dujiaTried = true; InitDujiaDriver(); }
                if (!g_dujiaLoaded)
                    PushLog("[独家] 独家驱动不可用（后台子进程无法隐藏，仍在任务管理器可见）");
            }
            // 只隐藏一个后台子进程（绝不动主进程与当前进程），不碰 UI 窗口
            ULONG hid = FindBackgroundChild(GetCurrentProcessId());
            if (hid && g_dujiaLoaded && g_dujia_three && g_dujia_three(hid, TRUE)) {
                s_hidden = true;
                s_hiddenPid = hid;
                char buf[160];
                snprintf(buf, sizeof(buf), "[独家] 已隐藏后台子进程 pid=%lu（驱动级）", hid);
                PushLog(buf);
            } else {
                s_hidden = true;
                s_hiddenPid = 0;
                PushLog("[独家] 未找到可隐藏的后台子进程（仅记录隐藏状态）");
            }
        }
        return;
    }

    // 独立模式：驱动隐藏自身进程
    if (!g_dujiaLoaded || !g_dujia_three) {
        // 未加载时自动尝试加载（不依赖 UI 的"连接独家驱动"按钮；
        // 驱动 exe 已安装时 one() 成功即就绪）
        PushLog("[独家] 驱动未连接，自动加载…");
        if (!InitDujiaDriver() || !g_dujia_three) {
            PushLog("[独家] 隐藏失败: 独家驱动加载失败（请确认驱动已安装且以管理员运行）");
            return;
        }
    }
    ULONG pid = GetCurrentProcessId();
    BOOL bHide = (g_hiddenPid == 0);
    if (g_dujia_three(pid, bHide)) {
        g_hiddenPid = bHide ? pid : 0;
        char buf[96];
        snprintf(buf, sizeof(buf), bHide ? "[独家] 已隐藏自身进程 (pid=%lu)" : "[独家] 已恢复显示自身进程 (pid=%lu)", pid);
        PushLog(buf);
    } else {
        PushLog("[独家] three() 返回 FALSE：驱动未执行（请确认驱动已安装且以管理员运行）");
    }
}

static bool DujiaMove(int dx, int dy) {
    if (!g_dujiaLoaded || !g_dujia_tow) {
        static int warnCount = 0;
        if (++warnCount <= 3) PushLog("[独家] 移动失败: 驱动未就绪");
        return false;
    }
    MOUSE_INPUT_DATA data{};
    data.Flags = MOVE_RELATIVE;
    data.ButtonFlags = MOVE_NULL;
    data.LastX = dx;
    data.LastY = dy;
    if (!g_dujia_tow(&data)) {
        static int failCount = 0;
        if (++failCount <= 3) PushLog("[独家] tow() 返回 FALSE（驱动未执行移动）");
        return false;
    }
    return true;
}

static void DujiaButton(USHORT buttonFlag) {
    if (!g_dujiaLoaded || !g_dujia_tow) return;
    MOUSE_INPUT_DATA data{};
    data.Flags = MOVE_RELATIVE;
    data.ButtonFlags = buttonFlag;
    if (!g_dujia_tow(&data)) {
        static int failCount = 0;
        if (++failCount <= 3) PushLog("[独家] tow() 返回 FALSE（按键未执行）");
    }
}

int GetMoveMethod() { return (int)g_moveMethod.load(); }
bool DujiaDriverReady() { return g_dujiaLoaded; }

void SetMoveMethod(int method) {
    // 选择只更新路由；硬件初始化仅由显式连接请求执行。
    g_moveMethod.store(method == 1 ? MoveMethod::MAKCU : method == 2 ? MoveMethod::DUJIA : MoveMethod::KMBOX);
}

namespace {
struct MoveJob {
    bool valid = false;
    int dx = 0, dy = 0;
    float dist = 0.f;
    bool fire = false;
    int device = -1;
    unsigned long long epoch = 0;
    int fireButton = VK_LBUTTON;
    double curveBudgetMs = 8.0;
};
std::mutex g_moveMtx;
std::mutex g_moveExecution;
std::mutex g_moveLifecycle;
std::condition_variable g_moveCv;
MoveJob g_moveJob;
bool g_moveQuit = false;
bool g_moveActive = false;
std::atomic<bool> g_moveAccepting{false};
std::atomic<unsigned long long> g_moveEpoch{1};
std::thread g_moveThread;
thread_local int g_jobDevice = -1;
thread_local unsigned long long g_jobEpoch = 0;
thread_local int g_jobFireButton = VK_LBUTTON;
thread_local int g_jobPressedButton = 0;
thread_local double g_jobCurveBudgetMs = 8.0;
std::chrono::steady_clock::time_point g_lastMoveInput{};
}

static bool MoveCancelled() {
    return !g_moveAccepting.load() || g_jobEpoch != g_moveEpoch.load() ||
        g_jobDevice != g_outputDevice.load() || g_jobDevice != GetMoveMethod();
}

static bool MoveDelay(int milliseconds) {
    std::unique_lock<std::mutex> lock(g_moveMtx);
    return !g_moveCv.wait_for(lock, std::chrono::milliseconds((std::max)(0, milliseconds)),
        [] { return MoveCancelled(); });
}

using MoveClock = std::chrono::steady_clock;
static inline void DoMouseMove(int dx, int dy);
static MoveClock::time_point MoveDeadline(MoveClock::time_point start, double ms) {
    return start + std::chrono::duration_cast<MoveClock::duration>(
        std::chrono::duration<double, std::milli>(ms));
}

static bool MoveWaitUntil(MoveClock::time_point deadline) {
    std::unique_lock<std::mutex> lock(g_moveMtx);
    return !g_moveCv.wait_until(lock, deadline, [] { return MoveCancelled(); });
}

// Sample the original path on an absolute timeline. Device I/O consumes this
// budget too; skip overdue samples instead of adding one sleep per packet.
template<class Sample>
static void PlayCurve(int total_dx, int total_dy, double durationMs, int steps,
                      MoveClock::time_point start, Sample sample) {
    int lx = 0, ly = 0;
    for (int i = 1; i <= steps; ++i) {
        if (i > 1 && durationMs > 0) {
            const double elapsed = std::chrono::duration<double, std::milli>(MoveClock::now() - start).count();
            const double progress = (std::clamp)(elapsed / durationMs, 0.0, 1.0);
            i = (std::max)(i, 1 + static_cast<int>(progress * (steps - 1)));
            if (!MoveWaitUntil(MoveDeadline(start, durationMs * (i - 1) / (steps - 1)))) return;
        }
        if (MoveCancelled()) return;
        double x, y;
        sample(static_cast<double>(i) / steps, x, y);
        const int nx = i == steps ? total_dx : static_cast<int>(std::round(x));
        const int ny = i == steps ? total_dy : static_cast<int>(std::round(y));
        if (nx != lx || ny != ly) DoMouseMove(nx - lx, ny - ly);
        lx = nx; ly = ny;
    }
}

static bool SendMoveToDevice(int device, int dx, int dy) {
    if (device == 1) return makcu_mouse_move(dx, dy);
    if (device == 2) return DujiaMove(dx, dy);
    if (device == 0) return kmNet_enc_mouse_move((short)dx, (short)dy) == 0;
    return false;
}

static void SendButtonToDevice(int device, int button, int state) {
    unsigned char mask=0;USHORT down=0,up=0;
    switch(button){
    case VK_LBUTTON: mask=MAKCU_BTN_LEFT;down=0x0001;up=0x0002;break;
    case VK_RBUTTON: mask=MAKCU_BTN_RIGHT;down=0x0004;up=0x0008;break;
    case VK_MBUTTON: mask=MAKCU_BTN_MIDDLE;down=0x0010;up=0x0020;break;
    case VK_XBUTTON1: mask=MAKCU_BTN_SIDE1;down=0x0040;up=0x0080;break;
    case VK_XBUTTON2: mask=MAKCU_BTN_SIDE2;down=0x0100;up=0x0200;break;
    default:return;
    }
    if(device==1)makcu_mouse_button(mask,state);
    else if(device==2)DujiaButton(state?down:up);
    else if(device==0){
        switch(button){
        case VK_LBUTTON: kmNet_enc_mouse_left(state);break;
        case VK_RBUTTON: kmNet_enc_mouse_right(state);break;
        case VK_MBUTTON: kmNet_enc_mouse_middle(state);break;
        case VK_XBUTTON1: kmNet_enc_mouse_side1(state);break;
        case VK_XBUTTON2: kmNet_enc_mouse_side2(state);break;
        }
    }
}

static inline void DoMouseMove(int dx, int dy) {
    if (MoveCancelled()) return;
    if (dx == 0 && dy == 0) return;

    // ★ 诊断：前 3 次非零移动记录路由方式
    static int routeLogCount = 0;
    if (++routeLogCount <= 3) {
        const char* methodName = (g_moveMethod == MoveMethod::MAKCU) ? "MAKCU"
            : (g_moveMethod == MoveMethod::DUJIA) ? "独家" : "KMBox";
        char buf[80];
        snprintf(buf, sizeof(buf), "[路由] DoMouseMove #%d dx=%d dy=%d -> %s",
            routeLogCount, dx, dy, methodName);
        PushLog(buf);
    }

    SendMoveToDevice(g_jobDevice, dx, dy);
}

static inline void DoFireButton(int state) {
    if(state){
        if(MoveCancelled())return;
        g_jobPressedButton=g_jobFireButton;
        SendButtonToDevice(g_jobDevice,g_jobPressedButton,1);
    }else if(g_jobPressedButton){
        // Release the exact key/device captured by this job, even after cancel.
        const int pressed=g_jobPressedButton;g_jobPressedButton=0;
        SendButtonToDevice(g_jobDevice,pressed,0);
    }
}

// ============================================================
//  屏幕参数
// ============================================================
static int CENTER_X = 0;
static int CENTER_Y = 0;

static int GetCenterX() {
    if (CENTER_X <= 0) CENTER_X = GetSystemMetrics(SM_CXSCREEN) / 2;
    return CENTER_X;
}
static int GetCenterY() {
    if (CENTER_Y <= 0) CENTER_Y = GetSystemMetrics(SM_CYSCREEN) / 2;
    return CENTER_Y;
}

void CenterToScreen(float x, float y) {
    CENTER_X = (int)x;
    CENTER_Y = (int)y;
}

// ============================================================
//  全局可调参数 —— P_PID 算法（5参数：比例/微分/预测/速率/平滑）
//  默认值保持旧算法等效行为：kd 原为 kp（比例），速率原为硬编码 0.3
// ============================================================
float CFG_PROPORTION = 25.0f;
float CFG_KD = 25.0f;
float CFG_PREDICT_X = 1.0f;
float CFG_PREDICT_Y = 0.0f;
float CFG_RATE = 0.3f;
float CFG_SMOOTH = 9900.0f;

// 输出与死区（CFG_OUT_MAX = 0 表示无上限）
float CFG_DZ_DIST = 0.5f;
float CFG_OUT_MAX = 0.0f;

// 贝塞尔
float CFG_BEZ_DIST_THRESH = 50.0f;
int   CFG_BEZ_MIN_DELAY = 2;
int   CFG_BEZ_MAX_DELAY = 5;
int   CFG_BEZ_DURATION_MS = 10;
int   CFG_BEZ_STEP_MS = 3;
float CFG_BEZ_DEVIATION = 0.28f;
int   CFG_BEZ_DIRECT_THRESH = 8;
bool  CFG_BEZ_ENABLED = true;

// 神经网络曲线（默认关闭，与贝塞尔互斥）
bool  CFG_NN_ENABLED = false;
float CFG_NN_DIST_THRESH = 15.0f;   // 目标距离大于该值才启用曲线
int   CFG_NN_STEP_MS = 2;           // 曲线每步间隔 ms

// ============================================================
//  P_PID 实例
// ============================================================
static P_PID pidX, pidY;
static bool g_pid_initialized = false;

// 参数热更新追踪
static float s_last_proportion = -1;
static float s_last_kd = -1;
static float s_last_predict_x = -1, s_last_predict_y = -1;
static float s_last_rate = -1;
static float s_last_smooth = -1;

void ApplyPIDParams() {
    bool needReinit = false;
    if (CFG_PROPORTION != s_last_proportion ||
        CFG_KD != s_last_kd ||
        CFG_PREDICT_X != s_last_predict_x || CFG_RATE != s_last_rate ||
        CFG_SMOOTH != s_last_smooth) {
        pidX.init(CFG_PROPORTION, CFG_KD, CFG_PREDICT_X, CFG_RATE, CFG_SMOOTH);
        needReinit = true;
    }
    if (CFG_PROPORTION != s_last_proportion ||
        CFG_KD != s_last_kd ||
        CFG_PREDICT_Y != s_last_predict_y || CFG_RATE != s_last_rate ||
        CFG_SMOOTH != s_last_smooth) {
        pidY.init(CFG_PROPORTION, CFG_KD, CFG_PREDICT_Y, CFG_RATE, CFG_SMOOTH);
        needReinit = true;
    }

    s_last_proportion = CFG_PROPORTION;
    s_last_kd = CFG_KD;
    s_last_predict_x = CFG_PREDICT_X; s_last_predict_y = CFG_PREDICT_Y;
    s_last_rate = CFG_RATE;
    s_last_smooth = CFG_SMOOTH;

    if (needReinit) g_pid_initialized = false;
}

static void EnsurePIDInit() {
    if (g_pid_initialized) return;
    ApplyPIDParams();
    g_pid_initialized = true;
}

// ============================================================
//  计时
// ============================================================
static auto          g_clk_start = std::chrono::steady_clock::now();
static unsigned long g_last_ms = 0;
static unsigned long ms_now() {
    return (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_clk_start).count();
}

// ============================================================
//  贝塞尔移动
// ============================================================
static void MoveBezier(int total_dx, int total_dy)
{
    if (MoveCancelled() || (total_dx == 0 && total_dy == 0)) return;
    if (std::hypot(static_cast<double>(total_dx), static_cast<double>(total_dy)) < CFG_BEZ_DIRECT_THRESH) {
        DoMouseMove(total_dx, total_dy);
        return;
    }
    const auto start = MoveClock::now();
    const int lo = (std::max)(0, CFG_BEZ_MIN_DELAY);
    const int hi = (std::max)(lo, CFG_BEZ_MAX_DELAY);
    const double requestedDelay = static_cast<double>(lo + rand() %
        (static_cast<unsigned long long>(hi) - lo + 1));
    const double requestedDuration = (std::max)(0, CFG_BEZ_DURATION_MS);
    const double requestedTotal = requestedDelay + requestedDuration;
    const double scale = requestedTotal > 0 ? (std::min)(1.0, g_jobCurveBudgetMs / requestedTotal) : 1.0;
    const double delay = requestedDelay * scale;
    const double duration = requestedDuration * scale;
    if (delay > 0 && !MoveWaitUntil(MoveDeadline(start, delay))) return;
    const int requestedSteps = (std::clamp)(CFG_BEZ_DURATION_MS / (std::max)(1, CFG_BEZ_STEP_MS), 2, 64);
    const int steps = (std::min)(requestedSteps, (std::max)(2, 1 + static_cast<int>(duration)));
    float r1 = ((rand() % 200 - 100) / 100.0f) * CFG_BEZ_DEVIATION;
    float r2 = ((rand() % 200 - 100) / 100.0f) * CFG_BEZ_DEVIATION;
    float p1x = total_dx * 0.25f - total_dy * r1, p1y = total_dy * 0.25f + total_dx * r1;
    float p2x = total_dx * 0.75f - total_dy * r2, p2y = total_dy * 0.75f + total_dx * r2;
    PlayCurve(total_dx, total_dy, duration, steps, MoveDeadline(start, delay),
        [=](double t, double& x, double& y) {
            const double u = 1.0 - t;
            x = 3 * u * u * t * p1x + 3 * u * t * t * p2x + t * t * t * total_dx;
            y = 3 * u * u * t * p1y + 3 * u * t * t * p2y + t * t * t * total_dy;
        });
}

// ============================================================
//  神经网络曲线移动（NeuralCurveModule.dll 动态加载）
//  与贝塞尔互斥：UI 强制二选一；目标 15px 外启用
// ============================================================
typedef void*  (*FnCurveCreate)();
typedef void   (*FnCurveInit)(void*, int, int, int, double, int);
typedef int    (*FnCurveMoveTo)(void*, double, double, double, double, double*, double*, int);

static HMODULE       g_nnDll = nullptr;
static FnCurveCreate g_nn_create = nullptr;
static FnCurveInit   g_nn_init = nullptr;
static FnCurveMoveTo g_nn_moveTo = nullptr;
static void*         g_nnHandle = nullptr;
static bool          g_nnReady = false;    // DLL 解析并创建成功
static bool          g_nnWarned = false;   // 加载失败只提示一次

static constexpr int   NN_MAX_POINTS   = 64;     // 输出点上限
static constexpr int   NN_TWEEN_POINTS = 20;     // 曲线插值点数（步数上限）

// 卸载曲线模块：内嵌内存模块走 NnFree，外部目录文件走 FreeLibrary
static void ShutdownNNModule() {
    if (g_nnDll) {
        if (NnIsEmbedded(g_nnDll)) NnFree();
        else FreeLibrary(g_nnDll);
        g_nnDll = nullptr;
    }
    g_nn_create = nullptr;
    g_nn_init = nullptr;
    g_nn_moveTo = nullptr;
    g_nnHandle = nullptr;
    g_nnReady = false;
}

static bool EnsureNNLoaded() {
    if (g_nnReady) return true;
    if (g_nnDll) return false;   // 已尝试但解析失败
    // 优先内嵌资源（编译进 aimlib，内存加载不落盘）；目录文件兜底
    g_nnDll = NnLoad();
    if (!g_nnDll) g_nnDll = LoadLibraryW(L"NeuralCurveModule.dll");
    if (!g_nnDll) {
        if (!g_nnWarned) { g_nnWarned = true; PushLog("[曲线] NeuralCurveModule 不可用（内嵌资源与本地文件均失败）"); }
        return false;
    }
    // 内嵌内存模块用 MemGetProcAddress，外部 DLL 用 GetProcAddress
    auto resolve = [&](const char* name) -> FARPROC {
        return NnIsEmbedded(g_nnDll) ? NnProc(name) : GetProcAddress(g_nnDll, name);
    };
    g_nn_create = (FnCurveCreate)resolve("Curve_Create");
    g_nn_init   = (FnCurveInit)resolve("Curve_Init");
    g_nn_moveTo = (FnCurveMoveTo)resolve("Curve_MoveTo");
    if (!g_nn_create || !g_nn_init || !g_nn_moveTo) {
        if (!g_nnWarned) { g_nnWarned = true; PushLog("[曲线] NeuralCurveModule 导出函数缺失"); }
        ShutdownNNModule();
        return false;
    }
    g_nnHandle = g_nn_create();
    if (!g_nnHandle) {
        if (!g_nnWarned) { g_nnWarned = true; PushLog("[曲线] 神经网络预测器创建失败"); }
        ShutdownNNModule();
        return false;
    }
    g_nn_init(g_nnHandle, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
        8 /*到达半径*/, 4.0 /*步长*/, NN_TWEEN_POINTS);
    g_nnReady = true;
    char buf[96];
    snprintf(buf, sizeof(buf), "[曲线] 神经网络曲线已加载（目标 %.0fpx 外启用）", (double)CFG_NN_DIST_THRESH);
    PushLog(buf);
    return true;
}

// 使用训练好的网络输出相对路径并逐段移动；路径终点精确落在 total_dx/total_dy
static void MoveNN(int total_dx, int total_dy)
{
    if (MoveCancelled() || (total_dx == 0 && total_dy == 0)) return;
    const auto start = MoveClock::now();
    if (!EnsureNNLoaded() || !g_nnHandle) {
        DoMouseMove(total_dx, total_dy);   // DLL 不可用时直接移动
        return;
    }
    double outX[NN_MAX_POINTS]{}, outY[NN_MAX_POINTS]{};
    int n = g_nn_moveTo(g_nnHandle, 0.0, 0.0,
        (double)total_dx, (double)total_dy, outX, outY, NN_MAX_POINTS);
    if (n <= 0 || n > NN_MAX_POINTS) { DoMouseMove(total_dx, total_dy); return; }
    double pathX[NN_MAX_POINTS + 1]{}, pathY[NN_MAX_POINTS + 1]{};
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(outX[i]) || !std::isfinite(outY[i]) ||
            std::abs(outX[i]) > 1000000 || std::abs(outY[i]) > 1000000) {
            DoMouseMove(total_dx, total_dy); return;
        }
        pathX[i + 1] = pathX[i] + outX[i];
        pathY[i + 1] = pathY[i] + outY[i];
    }
    // Keep fractional deltas, distributing any endpoint residual over the path.
    const double residualX = total_dx - pathX[n], residualY = total_dy - pathY[n];
    for (int i = 1; i <= n; ++i) {
        const double t = static_cast<double>(i) / n;
        pathX[i] += residualX * t;
        pathY[i] += residualY * t;
    }
    const double duration = (std::min)(g_jobCurveBudgetMs,
        static_cast<double>((std::max)(0, CFG_NN_STEP_MS)) * (n - 1));
    const int steps = (std::min)(n, (std::max)(2, 1 + static_cast<int>(duration)));
    PlayCurve(total_dx, total_dy, duration, steps, start,
        [&](double t, double& x, double& y) {
            const double position = t * n;
            const int index = (std::min)(static_cast<int>(position), n - 1);
            const double fraction = position - index;
            x = pathX[index] + (pathX[index + 1] - pathX[index]) * fraction;
            y = pathY[index] + (pathY[index + 1] - pathY[index]) * fraction;
        });
}

// ============================================================
//  移动输出线程 —— 鼠标设备 I/O 与推理循环解耦
//  推理线程(DisplayLoop)只计算 PID 输出并投递"最新作业"；
//  网络/串口调用、曲线绝对时间调度、开火等待全部在独立线程执行。
//  曲线保留 Qt 版的逐段相对移动，耗时按输入帧间隔收敛，避免逐点
//  等待和设备 I/O 叠加后降低输出吞吐；慢设备合并过期采样点。
//  采用"最新作业槽"：worker 忙碌时新作业直接覆盖旧作业，
//  永远执行最新 PID 输出（更旧的修正已过期，执行没有意义）。
// ============================================================
static void MoveWorkerLoop() {
    for (;;) {
        MoveJob job;
        {
            std::unique_lock<std::mutex> lk(g_moveMtx);
            g_moveCv.wait(lk, [] { return g_moveJob.valid || g_moveQuit; });
            if (g_moveQuit) break;
            job = g_moveJob;
            g_moveJob = {};
            g_moveActive = true;
        }
        {
            std::lock_guard<std::mutex> execution(g_moveExecution);
            g_jobDevice = job.device;
            g_jobEpoch = job.epoch;
            g_jobCurveBudgetMs = job.curveBudgetMs;
            g_jobPressedButton = 0;g_jobFireButton=job.fireButton;
            try {
                if (!MoveCancelled()) {
                    if (job.dx || job.dy) {
                        if (CFG_NN_ENABLED && job.dist > CFG_NN_DIST_THRESH)
                            MoveNN(job.dx, job.dy);
                        else if (!CFG_BEZ_ENABLED || job.dist < CFG_BEZ_DIST_THRESH)
                            DoMouseMove(job.dx, job.dy);
                        else MoveBezier(job.dx, job.dy);
                    }
                    if (job.fire && !MoveCancelled()) { DoFireButton(1); MoveDelay(2); }
                }
            } catch (...) {
                PushLog("[移动] 输出异常，本次任务已取消，不自动重试");
            }
            try { DoFireButton(0); } catch (...) { PushLog("[移动] 按键释放失败"); }
            g_jobDevice = -1;
        }
        {
            std::lock_guard<std::mutex> lock(g_moveMtx);
            g_moveActive = false;
        }
        g_moveCv.notify_all();
    }
}

static void ParkMoveOutput() {
    {
        std::lock_guard<std::mutex> lock(g_moveMtx);
        g_moveAccepting.store(false);
        g_moveEpoch.fetch_add(1);
        g_moveJob = {};
        g_lastMoveInput = {};
    }
    g_moveCv.notify_all();
    {
        std::unique_lock<std::mutex> lock(g_moveMtx);
        g_moveCv.wait(lock, [] { return !g_moveActive; });
    }
    // 等待已接受的单次手动调用结束；不持作业槽锁等待 I/O。
    std::lock_guard<std::mutex> execution(g_moveExecution);
}

void PauseMoveOutput() {
    std::lock_guard<std::mutex> lifecycle(g_moveLifecycle);
    ParkMoveOutput();
}

void ResumeMoveOutput() {
    std::lock_guard<std::mutex> lifecycle(g_moveLifecycle);
    std::lock_guard<std::mutex> lock(g_moveMtx);
    if (!g_moveThread.joinable()) {
        g_moveQuit = false;
        g_moveThread = std::thread(MoveWorkerLoop);
    }
    g_moveAccepting.store(true);
}

void ShutdownMoveOutput() {
    std::lock_guard<std::mutex> lifecycle(g_moveLifecycle);
    ParkMoveOutput();
    {
        std::lock_guard<std::mutex> lock(g_moveMtx);
        g_moveQuit = true;
    }
    g_moveCv.notify_all();
    if (g_moveThread.joinable()) g_moveThread.join();
}

int TestDeviceMove(int deviceType, int dx, int dy, std::string& error) {
    error.clear();
    if (deviceType < 0 || deviceType > 2 || dx < -32768 || dx > 32767 ||
        dy < -32768 || dy > 32767 || (!dx && !dy)) {
        error = "移动测试参数无效"; return 400;
    }
    std::unique_lock<std::mutex> lifecycle(g_moveLifecycle, std::try_to_lock);
    std::unique_lock<std::mutex> execution(g_moveExecution, std::try_to_lock);
    if (!lifecycle.owns_lock() || !execution.owns_lock()) {
        error = "输出线程忙碌"; return 409;
    }
    {
        std::lock_guard<std::mutex> lock(g_moveMtx);
        if (g_moveAccepting.load() || g_moveActive || g_moveJob.valid) {
            error = "请先暂停输出并等待作业结束"; return 409;
        }
    }
    try {
        if (g_moveQuit || g_outputDevice.load() != deviceType || GetMoveMethod() != deviceType ||
            (deviceType == 0 && !g_kmConnected.load()) ||
            (deviceType == 1 && !makcu_is_connected()) ||
            (deviceType == 2 && !DujiaDriverReady())) {
            error = "当前输出设备未就绪"; return 503;
        }
        if (!SendMoveToDevice(deviceType, dx, dy)) {
            error = deviceType == 1 ? makcu_get_last_error() : "设备移动调用失败";
            if (error.empty()) error = "设备移动调用失败，结果未确认";
            return 502;
        }
        return 0;
    } catch (const std::exception& e) {
        error = std::string("移动调用异常，结果未确认，请勿自动重试: ") + e.what();
    } catch (...) {
        error = "移动调用异常，结果未确认，请勿自动重试";
    }
    return 502;
}

static void EnqueueMove(int final_x, int final_y, float dist, bool fire, int fireButton) {
    {
        std::lock_guard<std::mutex> lock(g_moveMtx);
        const int device = g_outputDevice.load();
        if (!g_moveAccepting.load() || g_moveQuit || device < 0 || device != GetMoveMethod()) return;
        // Latest range decision wins: do not preserve a click after the target leaves.
        g_moveJob = {true, final_x, final_y, dist, fire, device, g_moveEpoch.load(), fireButton};
        const auto now = MoveClock::now();
        if (g_lastMoveInput != MoveClock::time_point{}) {
            const double interval = std::chrono::duration<double, std::milli>(now - g_lastMoveInput).count();
            // Reserve scheduling / transport headroom. Never replay an unbounded
            // backlog or multiply PID displacement to compensate for dropped frames.
            g_moveJob.curveBudgetMs = (std::clamp)(interval * 0.75, 1.0, 8.0);
        }
        g_lastMoveInput = now;
    }
    g_moveCv.notify_one();
}

// ============================================================
//  SendData —— 使用 P_PID 算法
// ============================================================
void SubmitAutoFire(bool fireNow,int fireButton) { EnqueueMove(0,0,0.f,fireNow,fireButton); }

void SendData(int targetX, int targetY, bool isfire, float /*sendOffset*/, int /*boxW*/, int fireButton)
{
    EnsurePIDInit();
    ApplyPIDParams();

    // ★ 诊断：前 5 次调用记录入参
    static int sendLogCount = 0;
    if (++sendLogCount <= 5) {
        char buf[100];
        snprintf(buf, sizeof(buf), "[SendData] #%d target=(%d,%d) center=(%d,%d) method=%d",
            sendLogCount, targetX, targetY, GetCenterX(), GetCenterY(), GetMoveMethod());
        PushLog(buf);
    }

    if (targetX < 100 || targetY < 100 || targetX > 3000 || targetY > 2000)
        return;

    float raw_x = (float)(targetX - GetCenterX());
    float raw_y = (float)(targetY - GetCenterY());
    float dist = sqrtf(raw_x * raw_x + raw_y * raw_y);

    // 死区：重置 PID，只投递开火点击（由输出线程执行，不阻塞推理）
    if (dist <= CFG_DZ_DIST) {
        pidX.reset(); pidY.reset();
        EnqueueMove(0, 0, dist, isfire, fireButton);
        return;
    }

    // 直接使用 P_PID 计算输出（内部处理滤波/预测/平滑）
    double step_x = pidX.update((double)raw_x);
    double step_y = pidY.update((double)raw_y);

    // ── 速度补偿：距离越远，速度倍率越高 ─────────────────────────
    double speedMultiplier = 1.0;
    if (dist >= 200.0f)       speedMultiplier = 2.0;
    else if (dist >= 100.0f)  speedMultiplier = 1.8;
    else if (dist >= 50.0f)   speedMultiplier = 1.4;
    else if (dist >= 20.0f)   speedMultiplier = 1.2;

    step_x *= speedMultiplier;
    step_y *= speedMultiplier;

    // 输出限幅（0 = 无上限）
    if (CFG_OUT_MAX > 0) {
        step_x = std::clamp(step_x, (double)-CFG_OUT_MAX, (double)CFG_OUT_MAX);
        step_y = std::clamp(step_y, (double)-CFG_OUT_MAX, (double)CFG_OUT_MAX);
    }

    int final_x = (int)round(step_x);
    int final_y = (int)round(step_y);

    // 移动和开火交给输出线程；曲线按帧预算调度，不阻塞推理。
    EnqueueMove(final_x, final_y, dist, isfire, fireButton);
}
