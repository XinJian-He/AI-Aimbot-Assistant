#pragma once
#include <opencv2/opencv.hpp>
#include <vector>
#include <memory>
#include "ModelClasses.h"
#include "onnxruntime_cxx_api.h"  // 本地 1.21.0 版本 (匹配 DirectML DLL)

struct Detection {
    cv::Rect2f box; // Keep subpixel precision through NMS and tracking.
    float conf;
    int classId;
};

struct match {
    int centerX;
    int centerY;
    float vx;
    float vy;
    float w;
    float h;
};

class Detector {
public:
    // 模型类型 (m_modelKind) 加载时根据输出张量形状自动检测, 无需手动选择:
    //   0 = 端到端输出 [1,N,6] 绝对像素 xyxy (YOLOv10 / YOLOv26)
    //   1 = anchors 输出 [1,4+nc,anchors] (YOLOv8 / YOLOv11)
    Detector(const std::string& modelPath);
    ~Detector();

    void SetInputSize(int size);
    void SetONNXDevice(int device) { m_onnxDevicePref = device; }  // 0=Auto 1=DML 2=CUDA 3=CPU
    int  GetDetectedInputSize() const { return m_detectedInputSize; }
    std::vector<Detection> Inference(const cv::Mat& frame);
    const std::vector<std::pair<int, std::string>>& GetClasses() const { return m_classes; }

private:
    std::vector<std::pair<int, std::string>> m_classes;

    // ---- ONNXRuntime ----
    Ort::Env ortEnv{ nullptr };
    Ort::Session ortSession{ nullptr };
    Ort::MemoryInfo ortMemInfo{ nullptr };

    std::vector<const char*> m_onnxInputNames;
    std::vector<const char*> m_onnxOutputNames;
    std::vector<std::string> m_onnxInputNameStrs;
    std::vector<std::string> m_onnxOutputNameStrs;

    // ---- ONNXRuntime 快速路径 (预分配输入 + 每帧 CreateTensor) ----
    std::vector<float> m_onnxInputData;            // 预分配 planar float 缓冲 (3*IS*IS)
    std::vector<float> m_onnxOutputData;            // 输出缓冲 (每帧 memcpy)
    int m_onnxOutputSize = 0;                       // 输出元素数
    std::array<int64_t, 4> m_onnxInputShape{};      // 预构建 shape {1,3,IS,IS}
    bool m_onnxWarmedUp = false;                    // 是否已完成首帧预热

    // 预处理查找表
    float m_lut[256];                               // i/255.0f 归一化表
    std::vector<int> m_contentMapX;                 // 内容区列映射: srcX = map[c]
    std::vector<int> m_contentMapY;                 // 内容区行映射: srcY = map[r]
    int m_lastSrcW = 0;                             // 跟踪源帧宽度变化, 懒重建映射表
    int m_lastSrcH = 0;

    // Letterbox 参数 (FastPreprocess 计算, PostProcess 消费)
    float m_scale = 1.0f;
    int m_padX = 0, m_padY = 0;
    int m_newW = 0, m_newH = 0;

    void BuildLUT();                                // 一次性初始化 m_lut
    void FastPreprocess(const cv::Mat& frame);      // 手写预处理 → m_onnxInputData

    int inputSize = 320;
    int m_detectedInputSize = 0;   // 从模型文件自动检测的输入尺寸（0=未检测到）
    int numClasses = 1;
    int numAnchors_ = 8400;   // 从 onnx 输出形状自动推导
    int m_modelKind = -1;     // 0=端到端[1,N,6](YOLOv10/v26) 1=anchors[1,4+nc,A](YOLOv8/v11), 加载时从输出形状自动检测
    int m_onnxDevicePref = 0;    // 0=Auto(DML→CUDA→CPU) 1=DirectML 2=CUDA 3=CPU
    std::string m_actualDeviceName = "unknown";  // DIAG: 实际加载的 EP 名称
    bool m_actualGpuOk = false;                  // DIAG: 是否成功加载 GPU EP

    void InitONNXRuntime(const std::string& modelPath);

    std::vector<Detection> InferenceONNXRuntime(const cv::Mat& frame);

    // 共用后处理：letterbox 参数 + 原始模型输出 → Detection 列表
    std::vector<Detection> PostProcessV10(const float* output, int numBoxes, int stride,
        int originalW, int originalH, float scale, int padX, int padY);
    std::vector<Detection> PostProcessV11(const float* output, int rowDim, int anchors,
        int originalW, int originalH, float scale, int padX, int padY);

    static float IoU(const cv::Rect2f& a, const cv::Rect2f& b);
    static void  NMS(std::vector<Detection>& dets, float iouThreshold);
};
