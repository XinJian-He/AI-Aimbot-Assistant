# 项目构建/部署速查手册（喂给 AI 用）

> 目标：拿到本文件后，能零考古地完成「改代码 → 构建 aimlib.dll → 部署 E:\QQLive\11.179.6377.0 → 打开验证」。
> 最后更新：2026-08-14（保存/修改配置按钮 bug 修复战役后）。

---

## 0. 权威源码目录（只有一个）

- **工作目录 = `E:\AIaim\最新稳定版\aim-DML - dll- QQLive`**（一切改动、构建、部署都基于它）
- `E:\AIaim\最新稳定版\AIM-DML - 备份` 是**旧副本**（源码停留在 8-12，没有今天的修复）——tlog 里出现的路径是历史遗留，**别在那里改代码**
- 项目目录历史上改过名：`AIM-DML - 备份` → `AIM-DML - DLL` → `AIM-DML - DLL- QQLive`（→ 现在的 `aim-DML - dll- QQLive`），构建产物里的 tlog 会同时记录这些旧名字

## 1. 工具链路径（全机唯一位置，别去 C:\Program Files 找）

| 组件 | 路径 |
|---|---|
| MSBuild (VS 17.14) | `E:\新建文件夹\MSBuild\Current\Bin\MSBuild.exe` |
| MSVC 工具集 | `E:\新建文件夹\VC\Tools\MSVC\14.44.35207\`（cl/link 在 `bin\Hostx64\x64\`，include 在 `include\`，lib 在 `lib\x64\`） |
| Windows SDK | `D:\Windows Kits\10\`（版本 `10.0.26100.0`，另有 18362；include 在 `Include\<ver>\{ucrt,um,shared,winrt,cppwinrt}`，lib 在 `Lib\<ver>\{ucrt,um}\x64`，rc.exe 在 `bin\<ver>\x64\`） |
| Qt | `E:\Qt\6.11.0\msvc2022_64\`（还有 llvm-mingw_64 / mingw_64 别用错） |
| Qt MSBuild 集成 | `%LOCALAPPDATA%\QtMsBuild\`（Qt VS Tools 3.5.0.0） |
| 项目依赖 | `<项目根>\deps\opencv\include`、`<项目根>\deps\onnxruntime-dml\include`（lib 同名） |
| 构建输出 | 最终产物 `x64\Release\`（aimlib.dll / ui-design.exe）；中间产物 `aimlib\x64\Release\`（.obj、tlog、qt\moc、qt\uic、qt\qmake\<hash>） |

## 2. 会话环境陷阱（90% 的时间都耗在这，先读！）

1. **qtpaths.exe 会挂死（最大的坑）**：Qt/MSBuild 的 QtPathsTask 会跑 `qtpaths.exe` 查询 Qt 路径。在非交互 shell（AI 会话）里它**打印完结果后永不退出**，且 taskkill 都杀不掉（报"没有运行实例"= 僵死）。MSBuild 会一直等 → **任何经 Qt/MSBuild 的构建在此会话都会挂**。
   - 快速判断：`tasklist | grep -i qtpaths` 出现 → 别走 MSBuild，直接走 §4 的手工构建
   - 用户自己的终端里 qtpaths 正常（普通 5 分钟构建），只有 AI 会话挂
2. **cmd.exe 无法接收中文路径**：bash→cmd 边界中文参数变乱码（`系统找不到指定的路径`）。所以 vcvars64.bat 等任何含中文路径的批处理都跑不起来。对策：用 Python subprocess（UTF-16 传参，可靠）或把路径放环境变量。
3. **PowerShell 长命令会挂**：`powershell -Command "复杂引号"` 可能挂死；简单 `-File xxx.ps1` 没问题。
4. **UAC 提权弹窗会卡死**：从 AI 会话 `Start-Process -Verb RunAs` 触发的 consent.exe 可能卡住且屏幕上不显示（用户看不到弹窗）。**对策：让用户在自己的输入框用 `!` 前缀运行 launch_deploy.ps1**，弹窗正常出现在用户桌面。
5. schtasks 创建的任务 `/TR` 引号解析脆，任务还可能删不掉（报"设备没有发挥作用"）——**用 /DISABLE 禁用**即可无害化，别纠缠删除。
6. 沙箱开关（dangerouslyDisableSandbox）对 qtpaths 挂死**无效**（不是沙箱问题），别浪费时间试。

## 3. 正常构建路径（用户在场时用，5 分钟）

```bash
"E:\新建文件夹\MSBuild\Current\Bin\MSBuild.exe" ui-design.sln -t:Build -p:Configuration=Release -p:Platform=x64 -nologo -v:minimal -m:1 -nodeReuse:false
```
- **必须串行 `-m:1`**（并行会 MSB3061 Qt DLL 拷贝竞争）
- `-nodeReuse:false` 防止僵尸 MSBuild 节点复用（本机有历史僵尸节点）
- 产物自动拷到 `x64\Release\`

## 4. AI 会话构建路径（绕开 qtpaths，已备好脚本）

**⚠️ 2026-08-14 晚：tlog 命令格式已被 MSBuild 重写为「裸源文件名 + 无引号中文路径」，build_manual.py 的旧正则会把 /I 参数吞掉导致 cl 静默不编译（D9021"没有执行操作"）→ 链接用的是旧 obj！** 现改为双脚本分工：
- **`python build_one.py <src...>`** —— 显式 cl 命令编译任意源文件（pid.cpp/proxy.cpp/aimapi_ui.cpp/integration.cpp/AimBackend.cpp 等；uidesign.cpp/aimapi_ui.cpp 自动带 Qt include）。**/MD + /Zc:__cplusplus 必须带**（其他 obj 是动态 CRT / Qt 要求）
- **`python build_link.py`** —— 纯重链（link tlog 最后一块 + MSVC/SDK /LIBPATH）。**已增强：自动补上 obj 目录里 tlog 未记录的 obj**（新增源文件如 embedded_nn.cpp 的 obj 也能链接上）
- 流程：改代码 → `build_one.py 相关源文件` → `build_link.py` → 校验导出 → 部署
- `build_manual.py` 的 cl 步骤已坏（别用）；tlog 解析仅剩 build_link.py 的 link 块（link 命令无空格路径问题，仍可靠）
- 校验：`dumpbin /exports x64\Release\aimlib.dll` 应有 **26 个 Aim_* 导出**（+Aim_ShowUIAsync）+ 4 个 SetupShell 原版导出

## 4.5 内嵌模块（封装dll.dll / NeuralCurveModule.dll）

- 两个小 DLL 编译期内嵌进 aimlib（加密 XOR 资源 + MemoryLoader 内存加载，不落盘）：
  - `embedded\封装dll_unpacked.dll` → `pack_dujia.py` → `embedded_dujia_data.h` → `embedded_dujia.cpp`（DujiaLoad，种子 0x9E3779B9）
  - `embedded\NeuralCurveModule_unpacked.dll` → `pack_dujia.py` → `embedded_nn_data.h` → `embedded_nn.cpp`（NnLoad，种子 0x51F57A93，数组名 kNnEnc）
- MSBuild 构建由 PreBuildEvent 自动重新打包；AI 会话手工改包：`python pack_dujia.py "embedded\NeuralCurveModule_unpacked.dll" embedded_nn_data.h kNnEnc 51F57A93`（改了一方必须同步另一方：种子/数组名在 py 与 cpp 两侧保持一致）
- 替换内嵌模块 = 替换 embedded\ 下的 _unpacked.dll 原件再重建（pid.cpp 仍保留目录文件兜底）
- MemoryLoader 支持 .pdata 异常表注册（RtlAddFunctionTable，x64 C++ 模块必需）
- 部署清单**不再含** 封装dll.dll / NeuralCurveModule.dll / opencv* / dxcompiler / dxil（已验证无运行时引用或已内嵌）

## 4.6 Qt 插件内嵌（platforms / imageformats）

- Qt 插件机制只认磁盘真实 DLL（QLibrary::load），无法内存加载 → **内嵌加密 + 运行时释放**：
  - `embedded\qt\` 下存放源件（platforms\qwindows.dll + imageformats\qjpeg.dll/qgif.dll；qico 无引用已砍，PNG/BMP 是 Qt6Gui 内置）
  - `python pack_dujia.py --dir embedded\qt embedded_qt_data.h 3A1F2C4D` → `embedded_qt_data.h`（kQtFiles 清单表）
  - `embedded_qt.cpp`（QtExtractPlugins，纯 Win32）：释放到 `%TEMP%\aimqt_<BUILD_SEED>\` 对应子目录
  - `aimapi_ui.cpp` EnsureApp：宿主无兼容 QApplication、自建前释放 + `QCoreApplication::addLibraryPath`
- 宿主（QQLive）版本匹配时走复用路径，插件根本不加载；内嵌释放只兜底自建路径
- **ui-design.exe 是独立 Qt exe，Release 已无插件目录**：运行需
  `set QT_QPA_PLATFORM_PLUGIN_PATH=E:\AIaim\最新稳定版\aim-DML - dll- QQLive\embedded\qt`
  （或把 embedded\qt 拷到 exe 旁改名 platforms/imageformats）

## 5. 部署（需要提权）

```powershell
# 用户在自己的输入框运行（! 前缀），然后点 UAC 弹窗的"是"
! powershell -NoProfile -ExecutionPolicy Bypass -File "E:\AIaim\最新稳定版\aim-DML - dll- QQLive\launch_deploy.ps1"
# 可选 -TargetDir E:\QQLive\11.179.6377.0（默认值就是这个）
```
- 调用链：launch_deploy.ps1（ASCII 提权包装）→ deploy_qqlive.ps1（提权后执行，**脚本必须保持纯 ASCII**：中文名用码点构造，如 封装dll.dll / 配置）
- deploy_qqlive.ps1 做：杀 QQLive 进程 → 还原 SetupShell_orig.dll（真品 167KB）→ aimlib.dll 改名为 SetupShell.dll 覆盖 → 拷 Qt 运行时（插件已内嵌 aimlib，运行时释放到 %TEMP%）→ 播种 model/background/配置（**已有用户配置绝不覆盖**）→ icacls 授 Users 写权限
- **日志**：`%TEMP%\deploy_qqlive.log`（提权控制台不可见，一切看这个文件）
- **部署验证**：`sha256(E:\QQLive\11.179.6377.0\SetupShell.dll) == sha256(x64\Release\aimlib.dll)`，且 SetupShell_orig.dll 保持 167472 字节
- **手动配置包**：`x64\Release\手动配置\` 是自包含部署套件（aimlib + 运行时库 + 模型/背景/配置 + 手动配置说明.txt），无脚本装机场景用；重新构建 aimlib 后需同步更新包内 aimlib.dll
- **回滚记录（2026-08-15）**：bootstrap 引导方案（Qt6/ORT/DML 内嵌进 SetupShell.dll）在 QQLive 实测失败（AimBackend::Init 内 4 次访问违规，模块级无法定位，本机复现正常——疑似 QQLive 进程环境特有），已整体回滚到 aimlib 直载版本。bootstrap 相关代码文件已删除；pack_dujia.py 的 --bundle 模式保留可复用

## 6. 打开验证

- 启动 `E:\QQLive\QQLive.exe`（或版本目录下对应主程序）→ 进程注入后弹出本程序 UI
- 验证点（本次修复）：
  1. 修改任意参数（如 PID 页比例）
  2. 切到"配置"页（切页不应回滚参数！）
  3. 点"配置另存为"→ 新文件内容应为修改后的值
  4. 下拉选中该配置 → 点"修改配置" → 覆盖后内容不变
  5. 配置文件在 `E:\QQLive\11.179.6377.0\配置\*.ini`（UTF-8，键=值，键名与 WriteConfigToFile/LoadConfigFile 一一对应）

## 7. 代码库关键事实（防止考古）

- **UI 是 uidesign.cpp（Qt，进程内）**，编译进 aimlib.dll；ui-design.exe 是独立版（不部署）
- **路径解析必须用 `g_proxyDir`**（proxy.cpp 在 PEB unlink 前记录）：GetModuleHandleExW(FROM_ADDRESS) 在 unlink 后失效会回退宿主 EXE 目录。uidesign.cpp 里 `extern "C" __declspec(selectany) wchar_t g_proxyDir[MAX_PATH]` 弱定义兜底（ui-design.exe 构建不含 proxy.cpp）
- 保存配置命名循环：`GetFileAttributesW` 对父目录不存在返回 ERROR_PATH_NOT_FOUND，统一"属性无效即视为可用文件名"，否则死循环
- **本次修复的 bug**（2026-08-14）：`uidesign.cpp onPageChanged` 切到配置页(index 6)恢复下拉选中项时没 blockSignals → setCurrentIndex 触发 cmbLoadCfg::currentIndexChanged → LoadConfigFile+SyncUIFromBackend 把未保存的修改静默回滚 → 保存/修改配置存的是旧值。修复：两处恢复循环（页5 cmbModelPath、页6 cmbLoadCfg/cmbLoadBg）加 blockSignals；保存/修改后重新选中目标文件（同样 blockSignals）
- **用户仍报"保存后配置不变"的根因（2026-08-14 晚确认）**：源码修复(17:21)已编译进 uidesign.obj(18:26)，但**部署目录 SetupShell.dll 是 16:23 的旧版**（修复前）——旧 DLL 里切配置页会回滚未保存修改。重新构建+部署后修复生效。无头验证：Aim_SetParam(proportion=66.6)+Aim_SaveConfigW → 文件写入 proportion=66.6 ✓
- **Home 键隐藏宿主 0x3B/宿主崩溃（2026-08-14 晚）**：aim 是注入宿主架构，GetCurrentProcessId()=QQLive 主进程。独家驱动 three() 对宿主大进程 VAD unlink → 驱动 bug（映像偏移 0x0a69 坏指针）→ 0x3B；ShowWindow(SW_HIDE) 宿主窗口 → 腾讯视频直接崩溃退出。**两条路都不可行**，用户坚持要 three() 进程隐藏（任务管理器找不到）→ 恢复 three() + 防御：`ToggleDujiaHideSelf` 未加载时自动 `InitDujiaDriver()`（不依赖 UI 连接按钮）+ **500ms 冷却**（注册表轮询异步有损握手，双击丢命令→g_hiddenPid 失配→重复 unlink 崩溃）+ 进程隐藏/恢复时同步切换 aim UI 窗口（`g_aimToggleUI` 弱指针 → aimapi_ui.cpp 的 Aim_ToggleUI，跨线程排队）。实测：真实按键多次切换无蓝屏，tasklist 中隐藏进程 PID 变 0 ✓
- **aim UI 跨线程创建（2026-08-14 晚）**：integration UiThread 在独立线程 new QApplication+uidesign 时，宿主已是 Qt 程序(QQLive 用部署的 Qt 6.11.0)则复用宿主 QApplication——独立线程 exec() 泵不到窗口事件，窗口 visible=1 但无 native 窗口。新增 `Aim_ShowUIAsync`（导出 #26）：宿主有 QApplication 时 BlockingQueuedConnection 投递到宿主 GUI 线程创建窗口；无则本线程创建需 RunLoop
- **build 脚本坑**：见 §4。tlog 的 CL 命令块源文件是**裸名无引号**（`... -Zc:externConstexpr UIDESIGN.CPP`），正则必须锚定行尾，否则吞掉中间 /I 参数
- **PowerShell 验证注意**：-File 脚本按 ANSI 解析（中文路径乱码）→ 路径走环境变量；P/Invoke aimlib 导出必须 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] + [MarshalAs(LPWStr)]（默认 stdcall+ANSI 会静默错乱）；SendInput 的 INPUT 结构 x64 必须 40 字节（含 MOUSEINPUT union），否则 rc=0 按键没注入
- tlog 目录：`aimlib\x64\Release\aimlib.tlog\`（CL.command.1.tlog / link.command.1.tlog，UTF-16LE，一条构建一个 `^obj列表` + 命令 块，**累积多次构建**，取最后一块）

## 8. 会话遗留物清单（2026-08-14 状态）

- 计划任务 `aimbuild` 存在但已被 **/DISABLE 禁用**（删除报设备错误，可不管；或用户手动在任务计划程序里删）
- 僵尸进程：若干 qtpaths.exe / consent.exe 可能残留（杀不掉，系统会清理）
- `build_manual.py` 保留在项目根（下次直接复用）
- 修复已编译进 `x64\Release\aimlib.dll`（18:27），**部署尚未完成**（等 UAC 批准）
