# 采集模式

采集入口统一为 cap_backend：0=GDI、1=DXGI、2=WGC、3=UDP、4=采集卡、5=串流。
UDP、采集卡、串流和 FrameGrabber/JpegCodec 源自 E:/AIaim/AI/AiMod/cap。

- UDP：MJPEG UDP，默认接收端口 7788；兼容参考项目 OBS UDP 发送方式。
- 采集卡：复用 Media Foundation / DirectShow 枚举、格式选择和中心裁剪。设备/格式留空时自动选择；指定设备不可用时报错，不改选其他设备。
- 串流：TCP 默认端口 9339；使用参考项目 E:/AIaim/AI/FrameSender 发送端，协议保持 StreamProtocol.h 一致（原始 BGR / MJPEG）。不是通用 RTSP/RTMP URL 输入。
- DXGI/WGC/GDI 保留本项目实现及桌面后端降级顺序；外部源失败不会改采本机桌面。

修改模式、端口、采集卡设备或格式会暂停处理；再次开始时应用。暂停释放端口和采集卡。接收使用有界等待，空帧退让 2ms，避免空转。
原 image_source / camera_index 已移出参数表及页面；加载旧虚拟摄像头 INI 时迁移到采集卡模式，保留旧设备序号。新配置只保存新键。

原生界面通过 Aim_GetCaptureDevicesJson 枚举设备，通过 Aim_GetCaptureModesJson 枚举格式。Aim_GetCaptureBackend 返回实际已初始化模式，-1 表示未初始化。

本次按要求不执行 C++ 编译或硬件接收测试。手动编译后应分别验证桌面三模式、UDP/串流发送端连接、采集卡枚举与格式切换，以及暂停后端口/设备释放。原文件备份：backups/capture-modes-20260929。
