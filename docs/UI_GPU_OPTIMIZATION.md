# UI 空闲 GPU 开销优化

## 发现

- `src/ui/native_window.cpp` 原循环每次都提交 D3D11 绘制并 `Present(1,0)`，只有垂直同步，没有独立的帧率上限。静止页面仍随显示器刷新率重绘。
- `src/ui/app_ui.cpp` 原阴影叠加 20 个覆盖整个窗口的半透明圆角矩形，随后又绘制不透明背景，造成大量无效混合和像素填充。
- 连接灯持续呼吸、资源条持续插值；暂停时快照仍每 250 ms 创建一次异步任务，读取参数并扫描模型/配置目录。
- 侧栏 GPU 是整机各进程合计后的最繁忙物理引擎，并非 UI 进程专属占用。因此不能将侧栏的 5% 直接归因于 UI。

## 修改

- 输入后 600 ms 内、拖动或文本编辑期间：按用户要求提高至约 120 FPS，采用 8 ms 帧间隔（定时上限 125 FPS）；实际帧率仍受垂直同步、显示器刷新率和调度影响。
- 前台静止：200 ms，最高 5 FPS；非前台：500 ms，最高 2 FPS。
- 等待使用 `MsgWaitForMultipleObjectsEx`：输入消息可及时唤醒并重新计算期限，但不会绕过交互帧率上限；不增加全局高精度计时器。
- 最小化或隐藏时不绘制，只每 250 ms 检查后台操作完成情况。DXGI 报告遮挡时每 500 ms 用 `DXGI_PRESENT_TEST` 检测恢复，不重复提交完整绘制。DirectComposition 并不保证报告所有遮挡情况，普通非前台窗口仍按 2 FPS 绘制。
- 关闭接口发送唤醒消息，避免等待下一个空闲期限。
- 阴影改为 4 道外侧窄轮廓；曲线细分误差由 0.15 调至 0.30，保留抗锯齿。连接灯改为静态，资源条直接展示最新采样。
- 暂停、后台、隐藏时快照间隔为 1 秒；前台运行或操作期间保留 250 ms。采集/推理线程的速率不由这些 UI 帧率控制。

## 验证

Release x64 DLL 完整编译及后续增量编译成功，日志：`.scratch/ui-gpu-build/build.log`。构建保留已有警告；未修改解决方案或部署目录。工程文件随后清理了导致 VS 加载失败的重复项。

使用 `.scratch/ui-gpu-build/probe.cpp` 加载新 DLL，在默认无模型、暂停且设备未连接的状态下，完成最小化/恢复、F1–F7 页面切换、缩放、关闭/重开和关闭后端的冒烟检查。`probe.log` 记录 PASS。

测试进程使用 PDH 按 PID 过滤 GPU Engine，每阶段 5 次一秒采样。窗口未取得前台焦点，后台空闲的最繁忙引擎均值为 0.0134%，后台发送鼠标消息阶段 0.0096%，最小化 0.0000%。这些是本次后台测试值，**没有同条件旧版本基线，也不是前台性能降幅**。

复验：

1. 运行 `x64/Release/host_test.exe --ui`；后续修复默认构建路径后，已成功完整重新生成解决方案的 `Release | x64`，该目录的 DLL 已包含 UI 优化。早期独立验证 DLL 保留在 `.scratch/ui-gpu-build/aimlib.dll`。
2. 保持模型暂停、设备未连接，用任务管理器观察对应进程的 GPU，而非侧栏整机百分比。
3. 分别观察前台静止、鼠标操作、切到后台和最小化；旧版/新版必须保持同一窗口大小、显示器设置及其他 GPU 负载。
4. 检查滑块拖动、文本输入、主题与标签切换、最小化恢复及开始/暂停状态显示。前台真实交互手感与前后 GPU 降幅仍需此步骤验证。

原始 UI 源码备份：`backups/ui-gpu-20260930-220636/src/ui/`。

## Visual Studio 项目加载

后续默认 Release 构建还出现 C1083：种子头已生成，但 `/FI` 使用 `$(IntDir)` 相对路径，无法从各源码的包含搜索路径找到。已增加 `AimBuildSeedHeader`，使用 `MSBuild::NormalizePath` 统一生成和强制包含的绝对路径，兼容相对及绝对 `IntDir`。修复后按默认目录执行 `MSBuild aimlib.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64` 成功（退出码 0）；日志为 `.scratch/ui-gpu-build/default-release-build.log`，工程备份为 `backups/build-seed-path-20260930-222153/aimlib.vcxproj`。

用户提供的 VS 错误确认原因：`src\common\obfuscated_text.h` 在 `aimlib.vcxproj` 中作为 `ClInclude` 重复声明，IDE 拒绝加载，虽然命令行 MSBuild 能编译。

已删除多余的头文件项目项，同时清理完全重复的构建种子属性、Release 编译属性和 `GenerateObfuscationSeed` 目标。原工程备份在 `backups/ui-gpu-20260930-220636/aimlib.vcxproj`。未修改 `aimlib.sln` 或 `aimlib.vcxproj.user`。修复后在 VS 右键重新加载项目；若 IDE 保留旧状态，关闭解决方案并重新打开当前目录的 `aimlib.sln`。不需要删除 `.vs` 缓存。
