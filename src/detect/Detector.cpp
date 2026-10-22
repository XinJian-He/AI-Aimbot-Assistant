#define NOMINMAX
#include "Detector.h"
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dml_provider_factory.h>
#include <fstream>
#include <stdexcept>
#include <numeric>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

extern void PushLog(const std::string& s);

// 记录单帧"纯推理"耗时（不含预处理/后处理），单位 ms
static thread_local double g_lastPureInferMs = 0.0;

// ═══════════════════════════════════════════════════════════════
// SelectBestDmlGpu — 用 DXGI 枚举显卡，选 VRAM 最多的
// ═══════════════════════════════════════════════════════════════
static UINT SelectBestDmlGpu()
{
    UINT bestIdx = 0;
    UINT64 bestVram = 0;

    IDXGIFactory* factory = nullptr;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory);
    if (FAILED(hr) || !factory) {
        PushLog("[DIAG] CreateDXGIFactory failed, using DML adapter 0");
        return 0;
    }

    UINT idx = 0;
    IDXGIAdapter* adapter = nullptr;
    PushLog("[DIAG] Enumerating GPUs for DML...");
    while (factory->EnumAdapters(idx, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            char nameBuf[256];
            WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, nameBuf, (int)sizeof(nameBuf), nullptr, nullptr);
            UINT64 vramMb = desc.DedicatedVideoMemory / (1024 * 1024);

            // 跳过 Microsoft Basic Render Driver (软件渲染器)
            bool isBasic = (strstr(nameBuf, "Microsoft Basic") != nullptr) ||
                           (strstr(nameBuf, "Render") != nullptr && vramMb == 0);

            char buf[512];
            snprintf(buf, sizeof(buf), "[DIAG]   GPU[%u]: %s  VRAM=%lluMB  VID=0x%04X%s",
                idx, nameBuf, (unsigned long long)vramMb, desc.VendorId,
                isBasic ? " (SKIP)" : "");
            PushLog(buf);

            if (!isBasic && vramMb > bestVram) {
                bestVram = vramMb;
                bestIdx = idx;
            }
        }
        adapter->Release();
        idx++;
    }
    factory->Release();

    char buf[128];
    snprintf(buf, sizeof(buf), "[DIAG] DML selected GPU[%u] (%llu MB VRAM)", bestIdx, (unsigned long long)bestVram);
    PushLog(buf);
    return bestIdx;
}

Detector::Detector(const std::string& modelPath)
{
    InitONNXRuntime(modelPath);
}

Detector::~Detector()
{
}

// =============================================================
// ONNXRuntime 初始化 (DirectML)
// =============================================================
void Detector::InitONNXRuntime(const std::string& modelPath)
{
    ortEnv = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "yolov8");

    // ═══ 打印 onnxruntime.dll 路径 ═══
    {
        HMODULE h = GetModuleHandleA("onnxruntime.dll");
        if (h) {
            char path[MAX_PATH];
            GetModuleFileNameA(h, path, sizeof(path));
            char buf[1024];
            snprintf(buf, sizeof(buf), "[DIAG] onnxruntime.dll: %s", path);
            PushLog(buf);
        }
    }

    // ═══ Session Options ═══
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
    opts.DisableMemPattern();

    // DML metacommand 禁用 — 小模型下独立算子比融合算子更快
    opts.AddConfigEntry("ep.dml.disable_metacommands", "1");

    // 自动选择最佳 GPU 并注册 DML EP
    UINT gpuIdx = SelectBestDmlGpu();
    char buf[128];
    snprintf(buf, sizeof(buf), "[DIAG] DML adapter: %u", gpuIdx);
    PushLog(buf);
    OrtSessionOptionsAppendExecutionProvider_DML(opts, (int)gpuIdx);
    PushLog("[DIAG] DirectML EP registered");

    // ═══ 创建 Session ═══
#ifdef _WIN32
    // UTF-8 → UTF-16 (中文路径安全转换; 禁止逐字节扩展, 否则中文路径必乱码)
    std::wstring wpath;
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            wpath.resize(wlen - 1);
            MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, &wpath[0], wlen);
        }
    }
    ortSession = Ort::Session(ortEnv, wpath.c_str(), opts);
#else
    ortSession = Ort::Session(ortEnv, modelPath.c_str(), opts);
#endif

    ortMemInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // ═══ 获取输入/输出名称 ═══
    Ort::AllocatorWithDefaultOptions alloc;
    m_onnxInputNames.clear();
    m_onnxOutputNames.clear();
    m_onnxInputNameStrs.clear();
    m_onnxOutputNameStrs.clear();

    for (size_t i = 0; i < ortSession.GetInputCount(); i++) {
        auto name = ortSession.GetInputNameAllocated(i, alloc);
        m_onnxInputNameStrs.push_back(name.get());
    }
    for (size_t i = 0; i < ortSession.GetOutputCount(); i++) {
        auto name = ortSession.GetOutputNameAllocated(i, alloc);
        m_onnxOutputNameStrs.push_back(name.get());
    }
    for (auto& s : m_onnxInputNameStrs)  m_onnxInputNames.push_back(s.c_str());
    for (auto& s : m_onnxOutputNameStrs) m_onnxOutputNames.push_back(s.c_str());

    // ═══ 从模型自动推导参数 ═══
    auto outShape = ortSession.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    auto inShape  = ortSession.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();

    if (inShape.size() >= 4 && inShape[3] > 0) {
        m_detectedInputSize = (int)inShape[3];
        inputSize = m_detectedInputSize;
    }

    // ═══ 自动识别 YOLO 版本类型 (依据输出张量形状) ═══
    //   d2 == 6      → 端到端 [1, num_boxes, 6]: YOLOv10 / YOLOv26 (绝对像素 xyxy, PostProcessV10)
    //   否则          → anchors [1, 4+nc, anchors]: YOLOv8 / YOLOv11 (PostProcessV11)
    //   anchors 数量恒 ≥ 2100 (320 输入), 与 6 不可能混淆
    int d1 = 0, d2 = 0;
    if (outShape.size() == 3) {
        d1 = (int)outShape[1]; d2 = (int)outShape[2];
        if (d2 == 6) {
            m_modelKind = 0;    // YOLOv10 / YOLOv26 端到端
        } else {
            m_modelKind = 1;    // YOLOv8 / YOLOv11 anchors
            int nc = d1 - 4;
            if (nc > 0) { numClasses = nc; numAnchors_ = d2; }
        }
    }
    {
        char kb[256];
        snprintf(kb, sizeof(kb), "[DIAG] 自动识别模型类型: %s (输出 [1,%d,%d])",
            m_modelKind == 0 ? "端到端 (YOLOv10/v26)" :
            m_modelKind == 1 ? "anchors (YOLOv8/v11)" : "未知形状",
            d1, d2);
        PushLog(kb);
    }

    // Read class names from this session once; missing metadata is non-fatal.
    std::string names;
    int classCount = m_modelKind == 1 && d1 > 4 ? d1 - 4 : -1;
    try {
        auto metadata = ortSession.GetModelMetadata();
        auto read = [&](const char* key) {
            auto value = metadata.LookupCustomMetadataMapAllocated(key, alloc);
            return value ? std::string(value.get()) : std::string();
        };
        names = read("names");
        if (model_classes::NamesParser(names).Parse().empty()) names = read("classes");
        if (classCount < 0) {
            classCount = model_classes::NamesParser::Count(read("nc"));
            if (classCount < 0) classCount = model_classes::NamesParser::Count(read("num_classes"));
        }
    } catch (const std::exception& e) {
        PushLog(std::string("[模型] 类别元数据读取失败: ") + e.what());
    }
    m_classes = model_classes::Resolve(names, classCount);

    // 输出缓冲区
    m_onnxOutputSize = 1;
    for (auto d : outShape) {
        if (d < 0) d = 1;
        m_onnxOutputSize *= (int)d;
    }

    // ═══ 预分配输入缓冲 + LUT + 预热 ═══
    BuildLUT();
    const size_t planeSize = (size_t)inputSize * inputSize;
    m_onnxInputData.resize(3 * planeSize);
    m_onnxInputShape = { 1, 3, (int64_t)inputSize, (int64_t)inputSize };

    // ═══ DIAG: 打印推理后端信息 ═══
    {
        char b1[256], b2[256];
        snprintf(b1, sizeof(b1), "[DIAG] Model: input=[1,3,%d,%d] output_size=%d",
            inputSize, inputSize, m_onnxOutputSize);
        PushLog(b1);
        snprintf(b2, sizeof(b2), "[DIAG] Provider=DirectML Optimize=BASIC Threads=1 MemPattern=OFF MetaCmd=OFF");
        PushLog(b2);
        PushLog("[DIAG] === DML INFERENCE READY ===");
    }

    m_actualDeviceName = "DirectML";
    m_actualGpuOk = true;

    // ═══ 模型预热 (首帧跑一次空数据，完成 kernel 编译) ═══
    PushLog("[DBG] DML warmup...");
    {
        std::fill(m_onnxInputData.begin(), m_onnxInputData.end(), 0.0f);
        auto inTensor = Ort::Value::CreateTensor<float>(
            ortMemInfo, m_onnxInputData.data(), m_onnxInputData.size(),
            m_onnxInputShape.data(), m_onnxInputShape.size());
        const auto w0 = std::chrono::high_resolution_clock::now();
        try {
            ortSession.Run(Ort::RunOptions{nullptr},
                m_onnxInputNames.data(), &inTensor, 1,
                m_onnxOutputNames.data(), m_onnxOutputNames.size());
        } catch (...) {
            PushLog("[DBG] DML warmup failed (non-fatal)");
        }
        const auto w1 = std::chrono::high_resolution_clock::now();
        double wMs = std::chrono::duration<double, std::milli>(w1 - w0).count();
        char wbuf[128];
        snprintf(wbuf, sizeof(wbuf), "[DBG] DML warmup %.1f ms", wMs);
        PushLog(wbuf);
    }
}

// =============================================================
// SetInputSize
// =============================================================
void Detector::SetInputSize(int size)
{
    if (size <= 0)
        throw std::invalid_argument("inputSize 必须大于 0");
    inputSize = size;

    // 分配 FastPreprocess 用的 CPU 缓冲 + 初始化 LUT
    static bool s_lutBuilt = false;
    if (!s_lutBuilt) { BuildLUT(); s_lutBuilt = true; }

    const size_t planeFloats = (size_t)inputSize * inputSize;
    m_onnxInputData.clear();
    m_onnxInputData.resize(3 * planeFloats);
    m_lastSrcW = 0;
    m_lastSrcH = 0;

    // 更新预构建 shape
    m_onnxInputShape = { 1, 3, (int64_t)inputSize, (int64_t)inputSize };
    m_onnxWarmedUp = false;  // 尺寸变更后需要重新预热
}

// =============================================================
// IoU / NMS
// =============================================================
float Detector::IoU(const cv::Rect2f& a, const cv::Rect2f& b)
{
    float interX1 = std::max(a.x, b.x);
    float interY1 = std::max(a.y, b.y);
    float interX2 = std::min(a.x + a.width, b.x + b.width);
    float interY2 = std::min(a.y + a.height, b.y + b.height);
    float interW = interX2 - interX1;
    float interH = interY2 - interY1;
    if (interW <= 0 || interH <= 0) return 0.0f;
    float interArea = (float)(interW * interH);
    float unionArea = (float)(a.area() + b.area()) - interArea;
    return interArea / unionArea;
}

void Detector::NMS(std::vector<Detection>& dets, float iouThreshold)
{
    std::sort(dets.begin(), dets.end(),
        [](const Detection& a, const Detection& b) { return a.conf > b.conf; });
    std::vector<bool> suppressed(dets.size(), false);
    std::vector<Detection> kept;
    for (size_t i = 0; i < dets.size(); i++) {
        if (suppressed[i]) continue;
        kept.push_back(dets[i]);
        for (size_t j = i + 1; j < dets.size(); j++) {
            if (!suppressed[j] && dets[i].classId == dets[j].classId &&
                IoU(dets[i].box, dets[j].box) > iouThreshold)
                suppressed[j] = true;
        }
    }
    dets = std::move(kept);
}

// =============================================================
// Inference (ONNXRuntime via DirectML)
// =============================================================
std::vector<Detection> Detector::Inference(const cv::Mat& frame)
{
    if (frame.empty()) return {};

    const auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<Detection> result = InferenceONNXRuntime(frame);

    const auto t1 = std::chrono::high_resolution_clock::now();
    const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 简单滑动统计：每隔 N 帧打印一次平均耗时，避免刷屏
    static int s_frameCnt = 0;
    static double s_totalMsSum = 0.0;
    static double s_inferMsSum = 0.0;
    s_frameCnt++;
    s_totalMsSum += totalMs;
    s_inferMsSum += g_lastPureInferMs;
    if (s_frameCnt >= 60) {
        const char* modelName = (m_modelKind == 0) ? "YOLO(end2end v10/v26)" :
                                (m_modelKind == 1) ? "YOLO(anchors v8/v11)" : "YOLO(unknown)";
        char buf[256];
        snprintf(buf, sizeof(buf),
            "[TIMING] engine=DML model=%s  avg total=%.3fms (%.1f FPS)  avg pure-infer=%.3fms",
            modelName,
            s_totalMsSum / s_frameCnt, 1000.0 / (s_totalMsSum / s_frameCnt),
            s_inferMsSum / s_frameCnt);
        PushLog(buf);
        s_frameCnt = 0;
        s_totalMsSum = 0.0;
        s_inferMsSum = 0.0;
    }

    return result;
}

// =============================================================
// BuildLUT: 一次性初始化 1/255 归一化查找表
// =============================================================
void Detector::BuildLUT()
{
    for (int i = 0; i < 256; i++)
        m_lut[i] = static_cast<float>(i) / 255.0f;
}

// =============================================================
// FastPreprocess: 手写 LetterBox 预处理 (替代 OpenCV 全链路)
//   一次遍历完成: 缩放 + BGR→RGB + 归一化 + HWC→CHW
//   直接写入 m_onnxInputData (每帧由 CreateTensor 包装)
// =============================================================
void Detector::FastPreprocess(const cv::Mat& frame)
{
    const int srcW = frame.cols;
    const int srcH = frame.rows;
    const int dstW = inputSize;
    const int dstH = inputSize;

    // ── 1. 计算 LetterBox 参数 ────────────────────────────────
    m_scale = std::min(static_cast<float>(dstW) / srcW,
        static_cast<float>(dstH) / srcH);
    m_newW = static_cast<int>(srcW * m_scale);
    m_newH = static_cast<int>(srcH * m_scale);
    m_padX = (dstW - m_newW) / 2;
    m_padY = (dstH - m_newH) / 2;

    // ── 2. 平面指针 ─────────────────────────────────────────
    const size_t planeSize = (size_t)dstW * dstH;
    float* rPlane = m_onnxInputData.data();
    float* gPlane = m_onnxInputData.data() + planeSize;
    float* bPlane = m_onnxInputData.data() + planeSize * 2;

    // ── 3. 灰度填充 (LetterBox 背景 114/255) ────────────────
    const float gray = m_lut[114];
    if (m_padX > 0 || m_padY > 0 || m_newW < dstW || m_newH < dstH) {
        std::fill(rPlane, rPlane + planeSize, gray);
        std::fill(gPlane, gPlane + planeSize, gray);
        std::fill(bPlane, bPlane + planeSize, gray);
    }

    if (m_newW <= 0 || m_newH <= 0) return;

    // ── 4. 懒重建内容映射表 (源帧尺寸变化时) ────────────────
    if (srcW != m_lastSrcW || srcH != m_lastSrcH) {
        m_lastSrcW = srcW;
        m_lastSrcH = srcH;

        m_contentMapX.resize(m_newW);
        m_contentMapY.resize(m_newH);

        const float invScale = 1.0f / m_scale;
        for (int c = 0; c < m_newW; c++) {
            int sx = static_cast<int>(c * invScale);
            m_contentMapX[c] = (sx < srcW) ? sx : (srcW - 1);
        }
        for (int r = 0; r < m_newH; r++) {
            int sy = static_cast<int>(r * invScale);
            m_contentMapY[r] = (sy < srcH) ? sy : (srcH - 1);
        }
    }

    const int* mapX = m_contentMapX.data();
    const int* mapY = m_contentMapY.data();
    const uint8_t* srcData = frame.data;
    const int srcStep = static_cast<int>(frame.step);
    const int srcCh = frame.channels();  // 3 for BGR

    // ── 5. 内容区域填充: BGR→Planar RGB + LUT归一化 ────────
    //    4像素展开循环
    for (int r = 0; r < m_newH; r++) {
        const int srcY = mapY[r];
        const uint8_t* srcRow = srcData + srcY * srcStep;
        const int dstRowBase = (m_padY + r) * dstW + m_padX;

        int c = 0;
        const int newW4 = m_newW - 3;
        for (; c < newW4; c += 4) {
            const uint8_t* p0 = srcRow + mapX[c] * srcCh;
            const uint8_t* p1 = srcRow + mapX[c + 1] * srcCh;
            const uint8_t* p2 = srcRow + mapX[c + 2] * srcCh;
            const uint8_t* p3 = srcRow + mapX[c + 3] * srcCh;

            const int di = dstRowBase + c;
            // OpenCV Mat 默认 BGR: p[0]=B, p[1]=G, p[2]=R
            rPlane[di] = m_lut[p0[2]];
            gPlane[di] = m_lut[p0[1]];
            bPlane[di] = m_lut[p0[0]];
            rPlane[di + 1] = m_lut[p1[2]];
            gPlane[di + 1] = m_lut[p1[1]];
            bPlane[di + 1] = m_lut[p1[0]];
            rPlane[di + 2] = m_lut[p2[2]];
            gPlane[di + 2] = m_lut[p2[1]];
            bPlane[di + 2] = m_lut[p2[0]];
            rPlane[di + 3] = m_lut[p3[2]];
            gPlane[di + 3] = m_lut[p3[1]];
            bPlane[di + 3] = m_lut[p3[0]];
        }
        // 尾部 1-3 列
        for (; c < m_newW; c++) {
            const uint8_t* p = srcRow + mapX[c] * srcCh;
            const int di = dstRowBase + c;
            rPlane[di] = m_lut[p[2]];
            gPlane[di] = m_lut[p[1]];
            bPlane[di] = m_lut[p[0]];
        }
    }
}

// =============================================================
// 共用后处理: YOLOv10 [1,300,6]
// =============================================================
std::vector<Detection> Detector::PostProcessV10(const float* output, int numBoxes, int stride,
    int originalW, int originalH, float scale, int padX, int padY)
{
    std::vector<Detection> results;
    const float confThreshold = 0.25f;
    const float nmsThreshold = 0.45f;

    for (int i = 0; i < numBoxes; i++)
    {
        const float* ptr = output + i * stride;

        const float score = ptr[4];
        if (!std::isfinite(score) || score < confThreshold) continue;

        float x1 = ptr[0], y1 = ptr[1], x2 = ptr[2], y2 = ptr[3];
        if (!std::isfinite(x1) || !std::isfinite(y1) ||
            !std::isfinite(x2) || !std::isfinite(y2)) continue;

        if (x1 > x2) std::swap(x1, x2);
        if (y1 > y2) std::swap(y1, y2);

        x1 -= padX; y1 -= padY; x2 -= padX; y2 -= padY;
        x1 /= scale; y1 /= scale; x2 /= scale; y2 /= scale;

        x1 = std::max(0.f, std::min(x1, (float)(originalW - 1)));
        y1 = std::max(0.f, std::min(y1, (float)(originalH - 1)));
        x2 = std::max(0.f, std::min(x2, (float)(originalW - 1)));
        y2 = std::max(0.f, std::min(y2, (float)(originalH - 1)));

        if (x2 - x1 < 1.f || y2 - y1 < 1.f) continue;

        Detection det;
        det.box = cv::Rect2f(x1, y1, x2 - x1, y2 - y1);
        det.conf = score;
        det.classId = (int)ptr[5];
        results.push_back(det);
    }

    NMS(results, nmsThreshold);
    return results;
}

// =============================================================
// 共用后处理: YOLOv11 / YOLOv8 [1,4+nc,anchors]
// =============================================================
std::vector<Detection> Detector::PostProcessV11(const float* output, int rowDim, int anchors,
    int originalW, int originalH, float scale, int padX, int padY)
{
    std::vector<Detection> results;
    const float confThreshold = 0.25f;
    const float nmsThreshold = 0.45f;
    const int nc = rowDim - 4;

    const float* row_cx = output + 0 * anchors;
    const float* row_cy = output + 1 * anchors;
    const float* row_w = output + 2 * anchors;
    const float* row_h = output + 3 * anchors;

    for (int i = 0; i < anchors; i++)
    {
        float maxScore = -1.f;
        int   bestCls = 0;
        for (int c = 0; c < nc; c++) {
            float s = output[(4 + c) * anchors + i];
            if (s > maxScore) { maxScore = s; bestCls = c; }
        }
        if (!std::isfinite(maxScore) || maxScore < confThreshold) continue;

        float cx = row_cx[i], cy = row_cy[i];
        float bw = row_w[i], bh = row_h[i];
        if (!std::isfinite(cx) || !std::isfinite(cy) ||
            !std::isfinite(bw) || !std::isfinite(bh)) continue;

        float x1 = cx - bw * 0.5f, y1 = cy - bh * 0.5f;
        float x2 = cx + bw * 0.5f, y2 = cy + bh * 0.5f;

        x1 -= padX; y1 -= padY; x2 -= padX; y2 -= padY;
        x1 /= scale; y1 /= scale; x2 /= scale; y2 /= scale;

        x1 = std::max(0.f, std::min(x1, (float)(originalW - 1)));
        y1 = std::max(0.f, std::min(y1, (float)(originalH - 1)));
        x2 = std::max(0.f, std::min(x2, (float)(originalW - 1)));
        y2 = std::max(0.f, std::min(y2, (float)(originalH - 1)));

        if (x2 - x1 < 1.f || y2 - y1 < 1.f) continue;

        Detection det;
        det.box = cv::Rect2f(x1, y1, x2 - x1, y2 - y1);
        det.conf = maxScore;
        det.classId = bestCls;
        results.push_back(det);
    }

    NMS(results, nmsThreshold);
    return results;
}

// =============================================================
// ONNXRuntime 推理路径 (DirectML)
//   每帧 CreateTensor + 推理 + memcpy 输出 + 后处理
// =============================================================
std::vector<Detection> Detector::InferenceONNXRuntime(const cv::Mat& frame)
{
    // ── A. 快速预处理 (手写, 直接写入 m_onnxInputData) ──────
    const auto tPre0 = std::chrono::high_resolution_clock::now();
    FastPreprocess(frame);
    const auto tPre1 = std::chrono::high_resolution_clock::now();
    const double preMs = std::chrono::duration<double, std::milli>(tPre1 - tPre0).count();

    const int originalW = frame.cols;
    const int originalH = frame.rows;

    // ── B. 每帧创建输入 Tensor ────────
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        ortMemInfo,
        m_onnxInputData.data(),
        m_onnxInputData.size(),
        m_onnxInputShape.data(),
        m_onnxInputShape.size());

    // ── C. 推理 ──
    const auto tInfer0 = std::chrono::high_resolution_clock::now();
    auto outputs = ortSession.Run(
        Ort::RunOptions{nullptr},
        m_onnxInputNames.data(), &inputTensor, 1,
        m_onnxOutputNames.data(), m_onnxOutputNames.size());
    const auto tInfer1 = std::chrono::high_resolution_clock::now();
    g_lastPureInferMs = std::chrono::duration<double, std::milli>(tInfer1 - tInfer0).count();

    if (outputs.empty()) return {};

    // ── D. memcpy 输出到预分配缓冲 ────
    float* raw = outputs[0].GetTensorMutableData<float>();
    size_t elemCount = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
    if (elemCount > m_onnxOutputData.size())
        m_onnxOutputData.resize(elemCount);
    memcpy(m_onnxOutputData.data(), raw, elemCount * sizeof(float));

    auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    if (outShape.size() != 3) return {};
    int d1 = (int)outShape[1];
    int d2 = (int)outShape[2];
    const float* output = m_onnxOutputData.data();

    // ── E. 后处理 (按自动识别的模型类型分发) ────────────────
    //   d2 == 6 判断与加载时一致: 端到端 [1,N,6] → V10, 否则 anchors → V11
    const auto tPost0 = std::chrono::high_resolution_clock::now();
    std::vector<Detection> dets;
    if (d2 == 6)
        dets = PostProcessV10(output, d1, d2, originalW, originalH, m_scale, m_padX, m_padY);
    else
        dets = PostProcessV11(output, d1, d2, originalW, originalH, m_scale, m_padX, m_padY);
    const auto tPost1 = std::chrono::high_resolution_clock::now();
    const double postMs = std::chrono::duration<double, std::milli>(tPost1 - tPost0).count();

    // ── DIAG: 首 5 帧打印耗时分解 ──────────────────────────
    {
        static int s_diagFrame = 0;
        if (s_diagFrame < 5) {
            s_diagFrame++;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "[DIAG] Frame#%d  pre=%.2fms  infer=%.2fms  post=%.2fms  total=%.2fms  EP=%s",
                s_diagFrame, preMs, g_lastPureInferMs, postMs,
                preMs + g_lastPureInferMs + postMs, m_actualDeviceName.c_str());
            PushLog(buf);
        }
    }

    return dets;
}
