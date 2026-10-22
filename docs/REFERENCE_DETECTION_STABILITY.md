# 4.0detaQQLive 检测稳定性对照

参考目录：E:/AIaim/最新稳定版/4.0detaQQLive。

源码证据：include/detector/detection.h 使用浮点端点；src/detector/yolov8_detector.cpp 的 postprocess/NMS 保留浮点坐标；src/overlay/overlay_window.cpp 的 drawDetections 在最终绘制时转整数。src/aim/aim_bot.cpp 将本帧 detections 直接交给 overlay，速度估计没有回写检测框，因此参考版没有绘框用的时序平滑。

本次借鉴：当前 Detector::Detection 改为 Rect2f，两个后处理分支与 NMS 都保留浮点；类别筛选、距离排名和锁定框保留浮点；瞄点叠加偏移后只在整数输出边界取整；框绘制使用 OpenCV 的 1/16 像素坐标和抗锯齿。保留已修正的卡尔曼噪声参数和顺序类别优先级。

没有照搬参考版的跨类别 NMS：它会压掉重叠的不同类别，可能破坏用户设定的类别优先顺序。两版预处理都是最近邻 letterbox，未凭空改成不同缩放算法。

参考版根目录 config.yaml 是另一模型、0.5 置信度、256 输入/采集尺寸；当前保存配置为 sjz.onnx、0.25 置信度、320 采集尺寸。文件配置不等于正在运行的配置，不能据此声称代码改完后与参考版效果一致。需要同一模型、同一录像和一致参数对比。

未编译主项目或运行模型/硬件。原文件备份在 backups/reference-precision-20260929。
