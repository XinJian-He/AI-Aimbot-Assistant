# aimlib — Windows x64 实时视觉瞄准系统

<div align="center">

![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-blue.svg)
![ONNX Runtime](https://img.shields.io/badge/ONNX%20Runtime-DirectML-green.svg)
![OpenCV](https://img.shields.io/badge/OpenCV-4.11-red.svg)
![ImGui](https://img.shields.io/badge/Dear%20ImGui-1.92.9b-orange.svg)
![License](https://img.shields.io/badge/License-GPL--3.0-blue.svg)

</div>

---

## 目录

- [项目简介](#项目简介)
- [功能概览](#功能概览)
- [系统架构](#系统架构)
- [目录结构](#目录结构)
- [构建指南](#构建指南)
- [运行与集成](#运行与集成)
- [采集模式](#采集模式)
- [目标检测与跟踪](#目标检测与跟踪)
- [瞄准控制与输出](#瞄准控制与输出)
- [原生 ImGui 控制台](#原生-imgui-控制台)
- [配置与参数](#配置与参数)
- [测试](#测试)
- [部署](#部署)
- [文档索引](#文档索引)
- [说明与边界](#说明与边界)

---

## 项目简介

`aimlib` 是一个 Windows x64 平台的实时视觉瞄准系统，以 **DLL** 形式交付。它把「屏幕采集 → 目标检测 → 目标跟踪 → 瞄准计算 → 设备输出」串成一条低延迟流水线，并内置一套**原生 ImGui 控制台**用于参数调节、设备连接、配置管理和日志查看。

核心特点：

- **纯本机推理**：ONNX Runtime + **DirectML** 执行提供器，自动枚举并选择显存最大的 GPU，无需 CUDA / TensorRT。
- **六种采集后端**：GDI、DXGI Desktop Duplication、Windows Graphics Capture、UDP MJPEG、采集卡（Media Foundation / DirectShow）、TCP 串流。
- **双检测模式**：YOLO ONNX 模型检测，或 HSV 找色；配合匈牙利匹配 + 卡尔曼滤波的多目标跟踪。
- **多种输出设备**：KMBox Net、MAKCU 串口盒子、内嵌独家驱动；急停注入另支持系统 `SendInput`。
- **原生界面**：Win32 + D3D11 + DirectComposition 渲染，PNG 资源编译进 DLL，不依赖 Qt。
- **可嵌入**：提供 `Aim_*` C API、host_test 示例宿主，以及按宿主模块名自动启动的代理载荷模式。

> 当前源码目录：`aim-wyy-DML-imgui`。生产构建产物为 `x64\Release\aimlib.dll`。

---

## 功能概览

### 屏幕采集

| 后端 | 编号 | 说明 |
|------|------|------|
| GDI | `0` | `BitBlt` CPU 截图，兼容性兜底 |
| DXGI | `1` | `IDXGIOutputDuplication`，GPU 桌面复制，延迟最低 |
| WGC | `2` | `Windows.Graphics.Capture`，WinRT 现代截图接口 |
| UDP | `3` | 接收 MJPEG UDP 流，默认端口 `7788`，兼容 OBS 类发送端 |
| 采集卡 | `4` | Media Foundation / DirectShow 枚举设备与格式，中心裁剪 |
| 串流 | `5` | TCP 串流，默认端口 `9339`，使用 `StreamProtocol.h` 协议 |

- 外部源（UDP / 采集卡 / 串流）初始化失败时**不会**回退到本机桌面，避免误采。
- 修改采集模式、端口、设备或格式会先暂停处理链路，再次开始时应用；暂停时释放端口与采集设备。
- `FrameGrabber/` 提供 DirectShow 采集与设备枚举，`JpegCodec` 负责 MJPEG 编解码。

### 目标检测

- ONNX Runtime + DirectML，自动选择 GPU；支持 FP32 模型与 224/256/320 等输入尺寸。
- 自动识别两种 YOLO 输出布局：
  - `[1, N, 6]` 端到端输出（YOLOv10 / YOLOv26）→ 绝对像素 `xyxy + conf + class`
  - `[1, 4+nc, anchors]` anchors 输出（YOLOv8 / YOLOv11）→ 相对 `cxcywh + 类别分数`
- Letterbox 预处理、置信度过滤、NMS 去重、类别 ID 过滤。
- 从 ONNX 元数据读取 `names` / `classes`，支持 Python 字典、JSON 字典与列表；界面可刷新并查看模型类别。
- HSV 找色模式：双 HSV 区间、ROI 半径、最小轮廓面积、瞄准缩放，可独立于 AI 检测使用。

### 目标跟踪

- `KalmanP`：匈牙利匹配（IoU / 距离 / 形状代价）+ 每个轨迹 7 状态卡尔曼滤波。
- 轨迹需连续命中 `min_lock_frames` 才被确认；丢失后保留 `max_drop_frames`，并允许 `max_compensate_frames` 预测补偿。
- 类别优先级按配置顺序从左到右；高优先级类别可打断低优先级锁定，跟踪关联不跨类别继承 ID。
- 目标丢失后在候选框中选择离屏幕中心最近者重新捕获。

### 瞄准控制

- **P_PID** 五参数控制：比例 `proportion`、微分 `kd`、X/Y 预测权重 `predict_x/predict_y`、速率 `rate`、平滑 `smooth`。
- 死区 `dz`、输出限幅 `out_max`、Y 轴偏移 `y_off`、瞄准点高度百分比 `aim_pct`、自瞄半径 `aim_r`。
- **贝塞尔曲线移动**：可配置触发距离、最小/最大延迟、持续时间、步进、偏移量和直达阈值。
- **神经网络曲线**：内嵌 `NeuralCurveModule`，目标距离超过 `nn_dist` 时启用，与贝塞尔互斥。
- **自动开火**：支持「中心半径」与「目标框内缩比例」两种触发模式，开火按键可配置。
- **急停注入（Counter-Strafe）**：监听 A/D/W/S 并注入反向按键，支持 KMBox（`1`）、`SendInput`（`2`）、MAKCU（`3`）三种后端；松开扳机或丢失目标时释放。
- 移动与开火通过独立输出线程提交，推理线程不被设备 IO 阻塞。

### 设备输出

| 方式 | `device_type` | 说明 |
|------|---------------|------|
| KMBox Net | `0` | TCP 网络盒子，加密指令；支持鼠标移动、按键、滚轮与鼠标状态监听 |
| MAKCU | `1` | 串口盒子，Compatible SDK / negotiated 4M（115200 → 4000000），ASCII 协议 |
| 独家驱动 | `2` | 内嵌 `封装dll` 内存加载；提供移动、按键与进程隐藏接口 |

- KMBox 鼠标键读取 `kmNet_monitor_mouse_*`，断连或监听未开启时按松开处理。
- MAKCU 只接入 Compatible SDK 方案（编号 `2`）；旧配置中的方案 `1/3/4` 加载时自动迁移。
- 设备连接、移动测试均为显式异步操作；发送成功表示指令已提交，不等同于硬件执行确认。

### 宿主集成

- `Aim_*` C API（见 `include/aim/aimapi.h`）：生命周期、参数、遥测、日志、配置、UI、设备连接。
- 生产构建的 DLL 导出表由 `src/proxy/proxy.def` 固定为 `bsdiff` / `bspatchW`，不暴露 `Aim_*`。
- 代理模式：DLL 以 `SetupShell.dll` 名称被宿主加载时，转发原 DLL 的 4 个 COM 导出，并在工作线程启动载荷；模块名不匹配时（如 host_test 以 `aimlib.dll` 加载）需显式调用 `Aim_Init` / `Aim_Start`。
- `stealth` 模块提供 PEB 模块链表摘除与 PE 头擦除，用于降低模块枚举可见性。

---

## 系统架构

```
┌──────────────────────────────────────────────────────────────────┐
│  UI 线程：Win32 + D3D11 + DirectComposition（src/ui）            │
│  总览 / 自瞄 / 控制 / 找色 / 设备 / 配置 / 日志                  │
└───────────────────────────┬──────────────────────────────────────┘
                            │ Aim_* C API、参数快照、遥测、日志
┌───────────────────────────▼──────────────────────────────────────┐
│  AimBackend（src/aim）                                           │
│                                                                  │
│  CaptureLoop ──► DisplayLoop ──► MainLoop ──► MoveOutput 线程     │
│  采集线程         检测 + Kalman    瞄准决策      PID / 曲线 / 开火 │
└──────┬──────────────────┬───────────────────┬────────────────────┘
       │                  │                   │
┌──────▼─────────┐ ┌──────▼──────────┐ ┌──────▼──────────────────┐
│ 采集后端        │ │ ONNX Runtime    │ │ KMBox / MAKCU / 独家驱动 │
│ GDI/DXGI/WGC    │ │ DirectML EP     │ │ 急停：SendInput          │
│ UDP/Card/Stream │ │ KalmanP 跟踪    │ │                          │
└────────────────┘ └─────────────────┘ └──────────────────────────┘
```

**线程模型**

- 采集线程持续产出帧；检测/显示线程做推理、跟踪与叠加绘制；主控线程负责参数与生命周期；输出线程串行执行移动与开火；UI 在独立线程运行。
- 修改采集区域、模型或设备等资源性参数时，通过请求标志让工作线程在安全点停车，主控线程等待全部线程 `parked` 后再改动资源并放行，避免竞争与卡死。
- 后端状态与遥测使用原子快照，UI 只读快照，不直接获取配置锁。

---

## 目录结构

```
aim-wyy-DML-imgui/
├── .gitattributes                 # 行尾与二进制文件规则
├── .gitignore                     # 构建产物 / 预编译依赖 / 运行时数据
├── LICENSE                        # GNU General Public License v3.0
├── README.md
├── THIRD_PARTY_NOTICES.md         # 第三方组件与许可证说明
├── aimlib.sln                     # 解决方案：aimlib / host_test / orig_stub
├── aimlib.vcxproj                 # 主工程（x64 DLL）
├── host_test.vcxproj              # 调用示例工程（默认不参与解决方案构建）
├── orig_stub.vcxproj              # SetupShell 原 DLL 转发桩
│
├── include/
│   └── aim/
│       ├── aimapi.h               # 公开 C API
│       └── exports.h              # SCap_* 采集 C 接口
│
├── samples/
│   └── host_test/
│       └── host_test.cpp          # 示例宿主源码
│
├── src/
│   ├── aim/                       # 后端核心
│   │   ├── AimBackend.h/.cpp      # 单例后端、线程、全局参数、遥测
│   │   ├── aimapi.cpp             # 公开 C API 与参数映射实现
│   │   ├── exports.cpp            # SCap_* 采集 C 接口实现
│   │   ├── integration.cpp        # 后端与宿主/UI 的集成入口
│   │   ├── MemoryLoader.h/.cpp    # 内存加载 PE（含 .pdata 注册）
│   │   ├── TargetPriority.h       # 类别优先级与目标选择
│   │   └── recent_log.h           # 环形日志
│   │
│   ├── capture/                   # 采集
│   │   ├── ScreenCapture.h/.cpp   # 统一门面，按模式分发
│   │   ├── GDIScreenCapture.*     # GDI
│   │   ├── DXScreenCapture.*      # DXGI
│   │   ├── WGCScreenCapture.*     # WGC
│   │   ├── ObsCapture.*           # UDP MJPEG
│   │   ├── CardCapture.*          # 采集卡
│   │   ├── StreamCapture.*        # TCP 串流
│   │   ├── StreamProtocol.h       # 串流协议
│   │   ├── JpegCodec.*            # MJPEG 编解码
│   │   └── FrameGrabber/          # DirectShow / 设备枚举
│   │
│   ├── detect/                    # 检测与跟踪
│   │   ├── Detector.h/.cpp        # ONNX Runtime + DirectML
│   │   ├── KalmanFilter.h         # 匈牙利匹配 + 7 状态卡尔曼
│   │   ├── ModelClasses.h         # ONNX 类别元数据解析
│   │   ├── findcolor.h            # HSV 找色
│   │   └── all.h                  # 公共包含
│   │
│   ├── control/                   # 控制与输出
│   │   ├── pid.h/.cpp             # PID、贝塞尔、神经网络曲线、设备分发
│   │   ├── kmboxNet.h/.cpp        # KMBox Net 协议
│   │   ├── makcu*.h/.cpp          # MAKCU 适配层与 SDK 桥接
│   │   ├── raw_input.h/.cpp       # Raw Input 按键状态
│   │   ├── HidTable.h             # HID 键码表
│   │   └── my_enc.h/.cpp          # 通信加密
│   │
│   ├── embed/                     # 编译期内嵌模块
│   │   ├── embedded_dujia.*       # 封装dll（独家驱动）
│   │   └── embedded_nn.*          # NeuralCurveModule
│   │
│   ├── proxy/                     # 代理载荷
│   │   ├── proxy.cpp              # SetupShell.dll 伪装与载荷线程
│   │   ├── bscommon_relay.cpp     # COM 导出转发
│   │   ├── proxy.def              # 生产导出表
│   │   └── orig_stub.def          # 原 DLL 导出桩
│   │
│   ├── stealth/                   # 模块隐蔽
│   │   └── stealth.h/.cpp         # PEB unlink / PE 头擦除
│   │
│   ├── common/                    # 构建种子与混淆文本
│   │   ├── build_seed.h
│   │   └── obfuscated_text.h
│   │
│   └── ui/                        # 原生 ImGui 界面
│       ├── app_ui.cpp             # 七个标签页与交互
│       ├── ui_backend.h/.cpp      # 界面后端桥接
│       ├── native_window.cpp      # Win32 + D3D11 窗口
│       ├── parameter_schema.inc   # 参数元数据
│       ├── native_json.h          # JSON 读写
│       ├── resource_monitor.h     # 资源占用
│       ├── image_texture.h        # 纹理加载
│       └── assets.rc              # PNG 资源
│
├── deps/
│   └── README.md                  # 预编译依赖获取说明（SDK 本体不入库）
│
├── third_party/
│   ├── imgui/                     # Dear ImGui 1.92.9b（源码内置，MIT）
│   └── makcu-cpp/                 # MAKCU SDK（源码内置，GPL-3.0）
│
├── embedded/                      # 待内嵌的模块原件（构建期打包）
├── docs/                          # 设计与验证记录
├── deploy/                        # 部署脚本
├── scripts/                       # 打包 / 路径校验 / 审计工具
└── tests/                         # 独立测试与探针

# 构建输出（不提交）：x64/
```

> `deps/opencv/` 与 `deps/onnxruntime-dml/` 中的预编译 SDK 被 `.gitignore` 排除，首次构建前需按 [deps/README.md](deps/README.md) 准备。

---

## 构建指南

### 环境要求

| 组件 | 要求 |
|------|------|
| 操作系统 | Windows 10 / 11 x64 |
| IDE | Visual Studio 2022（v143 工具集） |
| C++ 标准 | C++20 |
| Windows SDK | 10 |
| Python | 3.x（可选，仅在替换内嵌模块后重新生成 `*_data.h` 时需要） |
| GPU | 支持 DirectML 的显卡（NVIDIA / AMD / Intel 均可） |

OpenCV 与 ONNX Runtime 的预编译 SDK **不随仓库分发**，首次构建前按 [`deps/README.md`](deps/README.md) 放入 `deps/`。项目**不使用** Qt / CUDA / TensorRT。

### 构建步骤

1. 按 [`deps/README.md`](deps/README.md) 准备 `deps/opencv/` 与 `deps/onnxruntime-dml/`。
2. 用 Visual Studio 2022 打开 `aimlib.sln`。
3. 选择 `Release | x64`（调试可用 `Debug | x64`）。
4. 执行「清理解决方案」→「重新生成解决方案」。
5. 构建完成后检查输出目录 `x64\Release\`：

```
x64\Release\
├── aimlib.dll          # 主产物
├── SetupShell_orig.dll # 代理转发的原始 DLL 存根
├── onnxruntime.dll     # 由 CopyDlls 目标自动复制
├── DirectML.dll        # 由 CopyDlls 目标自动复制
└── models\             # 模型目录（按需放置）
```

> `host_test.exe` **不会**由解决方案默认生成，需单独构建 `host_test.vcxproj`（见下文「host_test 示例宿主」）。

### 构建要点

- **内嵌模块**：`src/embed/*_data.h` 已随仓库提供，构建不依赖 Python。仅当替换 `embedded/` 下的 DLL 后，才需要手动重新打包：
  ```bash
  python scripts/pack_dujia.py embedded/封装dll_unpacked.dll src/embed/embedded_dujia_data.h
  python scripts/pack_dujia.py embedded/NeuralCurveModule_unpacked.dll src/embed/embedded_nn_data.h kNnEnc 51F57A93
  ```
- **混淆种子**：Release 构建使用 `AimObfuscationSeed`（16 位十六进制）。未指定时自动生成 GUID 片段，并写入中间目录的 `obf_build_seed.h`；可用 `/p:AimObfuscationSeed=0123456789abcdef` 固定。
- **Debug 配置**使用 `/MD` 与 `_ITERATOR_DEBUG_LEVEL=0`，因为 `deps` 中的 `opencv_world.lib` 使用 Release CRT / STL ABI；不要改回 `/MDd`，否则会出现 `LNK2038`。
- 修改源文件列表或工程设置后必须重新生成，避免旧的 `.obj` 参与链接。

---

## 运行与集成

### host_test 示例宿主

`host_test.exe` 演示 `aimlib.dll` 的加载、初始化、配置与 UI 生命周期：

```bash
host_test.exe                  # 默认 UI 模式：打开原生 ImGui 窗口，关闭后退出
host_test.exe --ui             # 显式 UI 模式
host_test.exe --headless       # 无头模式：加载配置 → 开始 → 轮询遥测 8 秒 → 暂停
host_test.exe --cfg <ini>      # 无头模式指定配置文件
host_test.exe --save <path>    # 配置保存测试
```

无头模式默认尝试加载 EXE 同目录的 `配置\aim_config.ini`，未设置 `model_path` 时使用 `model\aim_model.onnx`。

> `host_test` 默认**不参与解决方案构建**：生产构建的 `proxy.def` 只导出 `bsdiff` / `bspatchW`，而 `host_test` 直接调用源码级 `Aim_*` API。它作为调用示例保留；如需实际链接，请在 `proxy.def` 中加入 `Aim_*` 导出，或使用单独的 API 导出配置。

### 作为 DLL 集成

- 源码接口见 `include/aim/aimapi.h`，调用顺序：

```c
Aim_Init();                    // 初始化后端（不自动连接设备）
Aim_ShowUI();                  // 可选：打开原生界面
Aim_SetParam("conf", "0.25");  // 参数键名与 INI 一致
Aim_Start();                   // 开始/继续
Aim_Telemetry t = Aim_GetTelemetry();
Aim_Pause();
Aim_CloseUI();                 // 若打开过界面
Aim_Shutdown();                // 退出前必须调用
```

- 不要在 `DllMain` / loader lock 内调用 `Aim_Shutdown`、`Aim_CloseUI` 等会等待线程的接口。
- `Aim_ShowUI` 在窗口就绪后返回；`Aim_CloseUI` 等待当前 UI 操作结束；关闭窗口本身不改变宿主拥有的后端生命周期。
- 生产构建的导出表由 `proxy.def` 固定为 `bsdiff` / `bspatchW`，`Aim_*` 仅作为源码内接口使用；代理模式仅在模块名为 `SetupShell.dll` 或设置环境变量 `AIM_PAYLOAD=1` 时自动启动载荷。

### 配置与模型

- 配置文件：DLL 同目录 `配置\aim_config.ini`，键名与 `Aim_SetParam` / `Aim_GetParam` 完全一致；兼容 UTF-8 BOM 与 GBK。
- 配置档案：界面「配置」页可新建、另存、加载、导入 INI，并保存 JSON 快照；档案列表扫描 DLL 旁的 `配置` 目录。
- 模型目录：界面扫描 `models` 与 `model` 目录；模型通过 ONNX Runtime 加载，类别名称从模型元数据读取。
- 界面参数为**草稿制**：修改后点击「应用参数」才逐项回读并持久化；应用失败会指出具体字段，不会把整页草稿标记为成功。

---

## 采集模式

统一参数为 `cap_backend`：

| 值 | 模式 | 关键参数 | 备注 |
|----|------|----------|------|
| `0` | GDI | `cap_r` | CPU 截图，兜底 |
| `1` | DXGI | `cap_r` | GPU 桌面复制，优先 |
| `2` | WGC | `cap_r` | WinRT 截图 |
| `3` | UDP MJPEG | `capture_udp_port` | 默认 `7788` |
| `4` | 采集卡 | `capture_device`、`capture_mode` | 留空自动选择；指定设备不可用时报错 |
| `5` | TCP 串流 | `capture_stream_port` | 默认 `9339`，非通用 RTSP/RTMP |

- `cap_r` 是以屏幕中心为原点的采集半径，默认 `112`，对应 `224×224` 输入区域。
- `max_capture_fps` 限制采集帧率，`0` 表示不限制（默认 `180`）。
- 采集模式、端口、设备或格式的变更在暂停后应用；暂停会释放端口与采集卡。
- 界面通过 `Aim_GetCaptureDevicesJson` / `Aim_GetCaptureModesJson` 枚举设备与格式，`Aim_GetCaptureBackend` 返回实际初始化的后端（`-1` 表示未初始化）。

---

## 目标检测与跟踪

### 模型要求

- 格式：`.onnx`（ONNX Runtime + DirectML）。
- 输入：`1×3×H×W`，`H/W` 为 224、256、320 等；程序按模型输入尺寸做 letterbox。
- 输出：自动识别以下两种布局，无需手动指定模型版本。

| 布局 | 典型模型 | 后处理 |
|------|----------|--------|
| `[1, N, 6]` | YOLOv10 / YOLOv26 | 绝对像素 `x1,y1,x2,y2,conf,class` |
| `[1, 4+nc, anchors]` | YOLOv8 / YOLOv11 | 相对 `cx,cy,w,h` + 类别分数，取最大类别 |

- 类别名称优先从 ONNX 元数据 `names` / `classes` 读取；类别数优先由输出形状推导，必要时读取 `nc` / `num_classes`。
- 置信度阈值由 `conf` 控制；`categories` 为逗号分隔的类别 ID，留空表示不选择目标。

### 跟踪流程

1. 检测结果按类别过滤并送入 `KalmanP`。
2. 匈牙利算法按 IoU / 距离 / 形状代价做帧间关联。
3. 每个轨迹用 7 状态卡尔曼 `[label, cx, cy, w, h, vx, vy]` 预测与更新。
4. 连续命中 `min_lock_frames` 后确认；丢失后保留 `max_drop_frames`，并允许 `max_compensate_frames` 预测补偿。
5. 按 `categories` 顺序选择目标；同类别优先保持原 ID，丢失超限后选择离屏幕中心最近者。

### HSV 找色

- 参数：`findcolor` 开关、`fc_radius` 扫描半径、两组 HSV 区间（`fc_h1/s1/v1` 与 `fc_h2/s2/v2`）、`fc_aim_scale`、`fc_min_area`。
- 流程：ROI 内颜色阈值 → 轮廓提取 → 最小面积过滤 → 计算瞄准点。
- 可与 AI 检测配合，也可独立使用。

---

## 瞄准控制与输出

### 控制参数

| 参数 | 作用 |
|------|------|
| `proportion` | 比例增益 |
| `kd` | 微分增益 |
| `predict_x` / `predict_y` | X / Y 轴预测权重 |
| `rate` | 输出速率 |
| `smooth` | 平滑系数 |
| `out_max` | 单次输出限幅 |
| `dz` | 死区（像素） |
| `y_off` | Y 轴瞄准偏移 |
| `aim_pct` | 瞄准点在目标框内的高度百分比 |
| `aim_r` | 自瞄半径：过滤距屏幕中心过远的检测框 |
| `conf` | 置信度阈值 |

### 曲线移动

- **贝塞尔**：`bez_enabled` 开启，`bez_dist` 触发距离，`bez_min_d` / `bez_max_d` 延迟范围，`bez_dur` 持续时间，`bez_step` 步进间隔，`bez_dev` 轨迹偏移，`bez_direct` 直达阈值。
- **神经网络曲线**：`nn_enabled` 开启，`nn_dist` 触发距离（默认 15px 外），`nn_step` 每步间隔。
- 两者互斥，界面会强制二选一。

### 自动开火

- `auto_fire`：总开关。
- `fire_mode = 0`：目标与屏幕中心距离小于 `fire_r` 时开火。
- `fire_mode = 1`：目标框按 `fire_box_ratio` 内缩，中心落入内缩框时开火。
- `auto_fire_button`：开火按键；`continuous_lock`：持续锁定模式。

### 设备与移动方式

| `device_type` | 设备 | 连接参数 |
|---------------|------|----------|
| `0` | KMBox Net | `km_ip`、`km_port`、`km_mac` |
| `1` | MAKCU | `makcu_port`（串口或 `AUTO`）、`makcu_scheme=2` |
| `2` | 独家驱动 | 内嵌模块，无需外部文件 |

- MAKCU 使用 Compatible SDK / negotiated 4M：`connect(port, true)` 从 115200 协商到 4000000；指定端口失败不换端口，仅 `AUTO` 时扫描。
- 移动测试要求当前连接代次匹配且处理链路暂停；每轴 `-32768..32767`，不能同时为零。
- 急停注入的 `cs_backend`：`0` 关闭、`1` KMBox、`2` SendInput、`3` MAKCU；松开扳机或丢失目标时自动释放。

---

## 原生 ImGui 控制台

界面在独立线程中运行，采用 Win32 + D3D11 + DirectComposition，PNG 资源编译进 DLL。共七个标签页：

| 标签页 | 内容 |
|--------|------|
| **总览** | 实时 FPS、锁定状态、置信度、距离、速度、推理/采集耗时、折线图 |
| **自瞄** | PID 参数、预测、死区、输出限幅、瞄准偏移、自瞄半径 |
| **控制** | 自动开火、开火模式、曲线移动、急停注入、热键、丢失/补偿帧 |
| **找色** | HSV 双区间、扫描半径、最小面积、瞄准缩放 |
| **设备** | 推理模型、模型类别、采集模式/设备/格式、KMBox/MAKCU 连接、移动测试 |
| **配置** | 新建/另存/加载/删除配置档案、导入 INI、JSON 快照 |
| **日志** | 后端实时日志（环形缓冲） |

- 连接、模型启动、参数应用、INI 保存均串行异步执行；操作期间相关控件禁用，日志页仍可查看。
- 设备/格式枚举与状态快照异步读取，避免渲染线程等待配置锁。
- 窗口缩放、主题、滑块/开关/导航动画由 `src/ui` 实现；资源占用数据来自后端遥测，不使用演示数字。

---

## 配置与参数

参数总数 60+，权威定义在 `src/ui/parameter_schema.inc` 与 `src/aim/aimapi.cpp` 的参数映射表中。主要分组：

| 分组 | 键名示例 |
|------|----------|
| 瞄准 | `proportion`、`kd`、`predict_x`、`predict_y`、`rate`、`smooth`、`out_max`、`dz`、`y_off`、`aim_pct`、`aim_r` |
| 开火 | `auto_fire`、`auto_fire_button`、`continuous_lock`、`fire_mode`、`fire_r`、`fire_box_ratio` |
| 跟踪 | `max_drop_frames`、`max_compensate_frames`、`min_lock_frames`、`vel_alpha`、`same_dist`、`conf`、`categories` |
| 曲线 | `bez_enabled`、`bez_dist`、`bez_min_d`、`bez_max_d`、`bez_dur`、`bez_step`、`bez_dev`、`bez_direct`、`nn_enabled`、`nn_dist`、`nn_step` |
| 采集 | `cap_backend`、`cap_r`、`capture_udp_port`、`capture_stream_port`、`capture_device`、`capture_mode`、`max_capture_fps` |
| 设备 | `device_type`、`km_ip`、`km_port`、`km_mac`、`makcu_port`、`makcu_baud`、`makcu_scheme` |
| 热键 | `hotkey1_enabled`、`hotkey1`、`hotkey2_enabled`、`hotkey2` |
| 找色 | `findcolor`、`fc_radius`、`fc_h1lo` … `fc_v2hi`、`fc_aim_scale`、`fc_min_area` |
| 推理 | `model_path`、`inference_engine`、`onnx_device` |

示例 `配置\aim_config.ini`：

```ini
proportion=0.2
kd=0.002
predict_x=1.2
predict_y=0.3
rate=1.0
smooth=0.18
out_max=150
dz=3
y_off=12
aim_pct=-30
conf=0.25
auto_fire=1
auto_fire_button=1
fire_mode=0
fire_r=3
aim_r=200
cap_backend=1
cap_r=112
max_capture_fps=180
model_path=model\aim_model.onnx
categories=0,5
device_type=0
km_ip=192.168.1.100
km_port=8888
```

> `inference_engine` 与 `onnx_device` 为旧配置兼容项，界面只读展示。

---

## 测试

测试均为独立程序，**不随解决方案默认构建**，需按目录内脚本或手动编译运行：

| 测试 | 内容 |
|------|------|
| `tests/target_classes_test.cpp` | 类别顺序、优先级与目标选择 |
| `tests/kalman_stability_test.cpp` | 卡尔曼跟踪稳定性 |
| `tests/kalman_replay.cjs` | 跟踪回放数据生成/校验 |
| `tests/kmbox_button_state_test.cpp` | KMBox 按键状态与去抖 |
| `tests/move_output/` | 移动输出、曲线调度（`run.ps1`） |
| `tests/makcu/` | MAKCU 传输层（`build_and_test.py` / `run.ps1`） |
| `tests/makcu_backend/` | MAKCU 后端适配（`run_tests.py`） |
| `tests/raw_input/` | Raw Input 按键状态（`run_tests.py`） |
| `tests/native_json_test.cpp` | 原生 JSON 读写行为 |
| `tests/recent_log_test.cpp` | 环形日志 |
| `tests/obfuscation/` | 混淆/构建种子检查（`run.ps1`） |
| `tests/capture_device_identity_test.cpp` | 采集设备身份识别 |
| `tests/dujia_probe/` | 独家驱动探针 |

运行结果以实际执行输出为准；仓库不记录「全部通过」之类的结论。

---

## 部署

`deploy/` 提供宿主部署链（以 QQLive 为例）：

```powershell
# 用户桌面执行，弹出 UAC 后确认
powershell -NoProfile -ExecutionPolicy Bypass -File "deploy\launch_deploy.ps1"
# 可选指定版本目录
powershell -NoProfile -ExecutionPolicy Bypass -File "deploy\launch_deploy.ps1" -TargetDir "E:\QQLive\11.179.6377.0"
```

- `launch_deploy.ps1`：ASCII 提权包装器，日志写入 `%TEMP%\launch_deploy.log`。
- `deploy_qqlive.ps1`：自动探测 `E:\QQLive` 下包含 `SetupShell.dll` 的最新版本目录；使用已验证的 `SetupShell_orig.dll` 备份，部署 `aimlib.dll` 为 `SetupShell.dll`，复制运行时并播种配置/模型（已有用户配置不覆盖）。
- 部署日志：`%TEMP%\deploy_qqlive.log`。
- `deploy/pack_qqlive.ps1`：打包部署目录。
- 验证：部署后的 `SetupShell.dll` 应与 `x64\Release\aimlib.dll` 哈希一致，原版 `SetupShell_orig.dll` 保持原始大小。

---

## 文档索引

| 文档 | 内容 |
|------|------|
| `docs/CAPTURE_MODES.md` | 六种采集模式、端口、暂停释放行为 |
| `docs/CAPTURE_UI_FIX.md` | 采集界面修复与实机验证步骤 |
| `docs/CARD_FPS.md` | 采集卡帧率与格式 |
| `docs/TARGET_CLASSES.md` | 类别顺序、优先级与模型元数据 |
| `docs/KALMAN_STABILITY.md` | 卡尔曼跟踪稳定性 |
| `docs/MAKCU_SCHEMES.md` | MAKCU 固定连接策略与移动测试 |
| `docs/MOUSE_HOTKEYS.md` | 鼠标/键盘热键 |
| `docs/KMBOX_HOLD_FIX.md` | KMBox 按住状态修复 |
| `docs/REFERENCE_DETECTION_STABILITY.md` | 检测稳定性参考 |
| `docs/UI_GPU_OPTIMIZATION.md` | UI GPU 占用优化 |
| `docs/UI_RESOURCES_MOTION.md` | UI 资源与动画 |
| `docs/IMGUI_MIGRATION.md` | Qt → 原生 ImGui 迁移说明 |
| `docs/LOG_RETENTION_AND_RANGES.md` | 日志保留与参数范围 |
| `docs/PLAYBOOK.md` | 历史构建/部署速查（Qt 时代，路径与流程已过时，仅作参考） |

---

## 许可证

本项目以 **GNU General Public License v3.0** 发布，完整文本见 [LICENSE](LICENSE)。

选择 GPL-3.0 的原因：`third_party/makcu-cpp` 采用 GPL-3.0，并直接编译进 `aimlib.dll`；Dear ImGui（MIT）、OpenCV（Apache-2.0）、ONNX Runtime（MIT）均与本许可证兼容。第三方组件、预编译 SDK 与内嵌二进制模块的说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

---

## 说明与边界

- 本 README 依据当前仓库源码与工程文件编写；历史文档（尤其是 `docs/PLAYBOOK.md`）可能描述已移除的 Qt 界面、旧目录名或旧构建链路，以源码与 `aimlib.vcxproj` 为准。
- 预编译依赖（OpenCV / ONNX Runtime）与构建产物不进入版本库，分别由 `.gitignore` 的 `deps/`、`x64/` 规则排除。
- 构建、硬件连接、采集实机验证与界面交互测试需要在本机执行；文档中的验证步骤不替代实际运行结果。
- 推理性能取决于 GPU、模型输入尺寸与驱动版本；DirectML 首次加载会包含会话初始化与 warmup 开销。
