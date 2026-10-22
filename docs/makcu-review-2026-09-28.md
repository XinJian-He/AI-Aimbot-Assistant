# MAKCU 连接和移动调用检查

检查日期：2026-09-28。按用户提供的 programming-engineering skill 的研究、复现、最小修复和回归检查方法执行。用户明确表示无实物盒子，因此测试结论限于主机代码与模拟串口，不声称已恢复真实鼠标移动。

## 结论

`km.move(dx,dy)\r\n` 的命令格式正确，与 MAKCU 官方 V3/V4 文档和两个公开库一致。现有 DCB 的 DTR/RTS 均为关闭，且已经存在接收线程；旧诊断脚本中相反的说法已经过时。不能把“格式错误”“没有接收线程”当作本版不动的原因。

原代码存在可以复现的通信缺陷，已修复：

| 缺陷与触发条件 | 原行为 | 修改后 |
|---|---|---|
| 握手先收到旧提示符，再收到版本 | 首次看到提示符便返回，错误判定握手失败 | 等待 MAKCU 身份及其后的提示符，支持分包 |
| WriteFile 成功但只写部分字节 | 余下字节丢失，命令缺少结尾，可能与下一条粘连 | 有界补写剩余字节；不能完成时中断连接 |
| WriteFile 失败或返回零字节 | 界面仍显示已连接，错误不进入 last_error | 保留具体错误，清除连接状态，后端更新失败状态并禁用输出 |
| ReadFile 因断线失败 | 接收线程继续循环，状态不变 | 结束读取，发布断线和错误 |
| 空闲同步读取 256 字节 | 每次可能等待 50ms，与同一句柄的写入争用 | 使用 Windows 立即返回超时组合，空闲时休眠 |
| 发送与关闭并发 | CloseHandle 不受发送互斥保护 | 串口生命周期与 I/O 分别加锁，先停止读线程再关闭 |
| int 坐标提前转 short | 65536 等值变成 0，超范围可能反向 | 在 int 上限幅到 -32768…32767，再格式化 |

连接时自动发送 `km.move(100,0)` 已移除，避免连接动作自带鼠标移动；诊断脚本通过 `-Move` 显式测试。移动函数返回“完整写入串口”的结果，前三次成功发送记录具体命令，不再把调用计数当成成功移动速率。

## 尚需实物确认的原因

1. **观察了错误的电脑或 USB 输出链路未就绪。** USB2 是串口控制端，USB1 才是鼠标输出端，USB3 需要接兼容的物理设备。双机模式下控制端本机光标不动是正常现象。旧脚本用控制端 GetCursorPos 和精确像素数判断故障，会误报；新脚本仅在 `-TargetOnThisPc` 时测本机光标，并说明加速度/屏幕边缘影响。
2. **V3 bypass 禁用了 USB 输出。** 官方明确说明 bypass(1/2) 禁用相应 USB 输出；串口仍可响应。脚本对可识别的 V3 查询该状态。V4 没有同名文本命令，未对未知固件自动发送该设置。
3. **双机热键状态未进入调用链。** `AimBackend.cpp` 的 `checkHotkey` 对 MAKCU 只读取控制端 `GetAsyncKeyState`，不解析 MAKCU 的物理按键流。当鼠标在 USB1 目标机且未启用连续锁定时，目标机按键不会触发控制端 `_doAim`，所以不会调用 `SendData`。本次没有擅自加入跨版本按键流实现；日志中完全没有 `[SendData]`/`[路由]` 时应先排查这一门控。代码注释“官方没有按键查询”已纠正。
4. **吞吐量与固件行为。** 115200 可以发送移动命令，并非无效档位，但官网给出的约 17 字节命令传输时间约 1.48ms，高频移动及多条点击命令会受带宽限制。4M 需要匹配盒子实际速率，不能只改 Windows DCB。未强制改变设备持久波特率。V4.028+ 的 `move` 有插值行为，`move_now` 是较新命令，不能无条件替换以免破坏旧固件兼容。

“锁轴一定导致注入无效”“必须切到 4M 才能移动”“串口写入成功等于 USB 执行成功”均没有足够证据作为本故障的确定结论。

## 验证

- `tests/makcu/run.ps1`：直接编译当前 `src/control/makcu.cpp`，仅替换 Win32 串口边界；不访问真实串口或驱动。
- 修改前：7 个场景中 6 个失败，覆盖握手分包、短写、读/写失败与空闲超时设置；串口配置失败清理已经通过。
- 修改后：12 个场景全部通过，包括仅命令回显不可视作握手、空端口参数、坐标限幅、断线后重连和并发关闭。
- `node --test tests/device_connect_ui.test.cjs`：8/8 通过。
- `makcu.cpp`、`pid.cpp`、`AimBackend.cpp` 以项目 C++20/x64 头文件与 MSVC 编译通过。已有宏重复定义、捕获头文件编码和未使用标签警告仍存在。
- 诊断脚本 PowerShell 语法检查通过；硬件脚本未执行。未重新链接、替换或部署 Release DLL。

## 后续实物复测

关闭占用 COM 口的主程序/官网工具，先运行（替换实际 COM 号）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\makcu_probe.ps1 -Port COM5 -Baud 115200 -Move
```

观察 USB1 对应电脑；同机接线时可加 `-TargetOnThisPc`。脚本输出版本、设备、echo 和适用的 bypass 信息，发送 +60/-60，并明确区分回显与实际执行。若盒子实际设为 4M，则使用 `-Baud 4000000`，脚本不更改盒子波特率。

## 来源

- [MAKCU 官方 API](https://www.makcu.com/en/api/)：V3/V4 版本差异、移动格式、echo、bypass、物理按键流。
- [MAKCU 官方端口和波特率说明](https://www.makcu.com/en/information/)：USB1/2/3、连接设备要求和传输时间。
- [MAKCU 官方故障排查](https://www.makcu.com/en/troubleshooting/)：串口、固件、供电及 USB 输出检查。
- [K4HVH/makcu-cpp 连接和命令实现](https://github.com/K4HVH/makcu-cpp/blob/9874efa96f58e7a768e0110d765f0fa072e7e25e/makcu-cpp/src/makcu.cpp)、[Windows 串口实现](https://github.com/K4HVH/makcu-cpp/blob/9874efa96f58e7a768e0110d765f0fa072e7e25e/makcu-cpp/src/serialport.cpp)：公开社区实现，未当作官方固件规范，也未复制其 GPL 实现到项目。
- [SleepyTotem/makcu-py-lib](https://github.com/SleepyTotem/makcu-py-lib/blob/c89ed736a977104fe2df78b0fa7afc0ba5516c03/connection.py)：交叉核对串口和传统波特率切换帧。
- [Windows COMMTIMEOUTS](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-commtimeouts)：ReadIntervalTimeout=MAXDWORD、其余读总超时为 0 时，ReadFile 立即返回已缓冲字节。

## 先前要求的 one/tow/three 静态检查

这些函数属于独家驱动分支，与 MAKCU 的串口移动分支独立。已对照本机参考 `DQDriver.h` 和实际内嵌 DLL 的导出/反汇编：导出确实为 `one`、`tow`、`three`，当前 BOOL 函数签名与参数类型匹配 x64 调用。`one`、`tow` 在 DLL 内部比较响应码 6 后返回 BOOL，不能把“one 返回 6”当作准确的公开返回契约。

`MOVE_RELATIVE=0x01` 与 Windows 标准 MOUSE_INPUT_DATA 的相对标志 0 不一致；封装 DLL 仅传递指针，不能证明内核驱动是否自行解释该值。在缺少驱动源码/硬件复测的情况下未盲改。`three` 的恢复分支忽略返回值、驱动卸载前未恢复隐藏状态，以及 detached 移动线程没有在设备切换前 join，仍是单独的驱动生命周期风险。本次未执行进程隐藏或真实驱动调用，也未改动其行为。
