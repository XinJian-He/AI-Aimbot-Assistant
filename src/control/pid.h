#pragma once
#define NOMINMAX
#include <atomic>
#include <string>

// ============================================================
//  MoveMethod 枚举
// ============================================================
enum class MoveMethod { KMBOX = 0, MAKCU = 1, DUJIA = 2 };
extern std::atomic<MoveMethod> g_moveMethod;
extern std::atomic<int> g_outputDevice; // -1: 尚未显式连接，禁止设备输出

// ============================================================
//  函数声明
// ============================================================
void SendData(int x, int y, bool fireNow, float sendOffset, int boxW, int fireButton = 1);
void SubmitAutoFire(bool fireNow, int fireButton);
void CenterToScreen(float x, float y);
void SetMoveMethod(int method);
int  GetMoveMethod();          // 当前移动方式 0=KMBox 1=MAKCU 2=独家驱动
bool DujiaDriverReady();       // 独家驱动是否已加载可用
void ApplyPIDParams();
bool InitMAKCUDriver(const char* portName, int baudRate = 115200);
void ShutdownMAKCUDriver();
void PauseMoveOutput();
void ResumeMoveOutput();
void ShutdownMoveOutput();
int TestDeviceMove(int deviceType, int dx, int dy, std::string& error);
// 按键状态读取（瞄准热键与找色右键共用），定义在 AimBackend.cpp：
//   MAKCU 只读本机 Raw Input，不混用系统状态或盒子监听；
//   KMBox 以盒子状态为权威（物理鼠标挂在盒子上，本机状态读不到它），盒子答不了
//         （-1：监听未开启，或超过 MONITOR_FRESH_MS 未收到数据）才回退系统状态；
//   其他后端回退系统状态。
// 0 与 -1 必须区分：0=松开，-1=未知。键盘 vk 不匹配盒子分支，走系统状态。
bool ReadHotkeyState(int vk);
// 仅显式连接选定方案；只有配置 AUTO 才扫描 COM 口（定义在 AimBackend.cpp）
bool EnsureMAKCUConnected(std::string* error = nullptr);
bool InitDujiaDriver(std::string* error = nullptr);
void ShutdownDujiaDriver();
void ToggleDujiaHideSelf();   // Home 键：切换隐藏/显示自身进程（需独家驱动）

// ============================================================
//  P_PID 可调参数 extern 声明（定义在 pid.cpp）—— 5参数：比例/微分/预测/速率/平滑
// ============================================================
extern float CFG_PROPORTION;
extern float CFG_KD;
extern float CFG_PREDICT_X;
extern float CFG_PREDICT_Y;
extern float CFG_RATE;
extern float CFG_SMOOTH;

// 输出与死区
extern float CFG_DZ_DIST;
extern float CFG_OUT_MAX;

// 贝塞尔
extern float CFG_BEZ_DIST_THRESH;
extern int   CFG_BEZ_MIN_DELAY;
extern int   CFG_BEZ_MAX_DELAY;
extern int   CFG_BEZ_DURATION_MS;
extern int   CFG_BEZ_STEP_MS;
extern float CFG_BEZ_DEVIATION;
extern int   CFG_BEZ_DIRECT_THRESH;
extern bool  CFG_BEZ_ENABLED;

// 神经网络曲线（与贝塞尔互斥，UI 下拉框单选）
extern bool  CFG_NN_ENABLED;
extern float CFG_NN_DIST_THRESH;   // 目标距离大于该值才启用曲线（默认 15px）
extern int   CFG_NN_STEP_MS;       // 曲线每步间隔 ms（默认 2）
