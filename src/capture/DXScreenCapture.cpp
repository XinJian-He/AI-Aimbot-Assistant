#define NOMINMAX
#include "DXScreenCapture.h"
#include <algorithm>
#include <intrin.h>
#include <thread>
#include <cstdio>

DXScreenCapture::DXScreenCapture()
    : pd3dDevice(nullptr)
    , pd3dContext(nullptr)
    , pDeskDupl(nullptr)
    , pDXGIOutput(nullptr)
    , pDXGIAdapter(nullptr)
    , pDXGIFactory(nullptr)
    , frameTexture(nullptr)
    , captureX(0)
    , captureY(0)
    , captureWidth(0)
    , captureHeight(0)
    , outWidth(0)
    , outHeight(0)
    , outChannels(3)
    , currentTexturePair(nullptr)
    , textureInitialized(false)
    , initialized(false)
    , updated(false)
    , sse42Supported(false)
    , useCenterMode(false)
    , screenWidth(0)
    , screenHeight(0)
    , frameCounter(0)
    , reinitCount(0) {

    sse42Supported = IsSSE42Supported();
}

DXScreenCapture::~DXScreenCapture() {
    Release();
}

bool DXScreenCapture::IsSSE42Supported() {
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    return (cpuInfo[2] & (1 << 20)) != 0;
}

inline bool DXScreenCapture::ValidateBMPParameters(const BYTE* srcData, int width, int height, int srcPitch) {
    return srcData && width > 0 && height > 0 && srcPitch > 0;
}

inline bool DXScreenCapture::IsResolutionFirstCall(int width, int height) {
    for (auto& rc : resolutionFirstCalls) {
        if (rc.IsFirstCall(width, height)) {
            return true;
        }
    }
    return false;
}

bool DXScreenCapture::InitializeDXGI() {
    if (initialized) return true;

    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pDXGIFactory);
    if (FAILED(hr)) {
        snprintf(lastError, sizeof(lastError),
            "CreateDXGIFactory1 失败 HRESULT=0x%08X (DXGI 1.1 不可用, 可能需要 Win8+)", (unsigned)hr);
        OutputDebugStringA(lastError);
        return false;
    }

    // 收集所有适配器诊断信息
    char diagBuf[1024] = {0};
    int diagLen = 0;

    for (UINT i = 0; pDXGIFactory->EnumAdapters1(i, &pDXGIAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 adapterDesc;
        pDXGIAdapter->GetDesc1(&adapterDesc);
        int vramMB = (int)(adapterDesc.DedicatedVideoMemory / (1024 * 1024));

        diagLen += snprintf(diagBuf + diagLen, sizeof(diagBuf) - diagLen,
            "[适配器%u] VRAM=%dMB ", i, vramMB);

        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        UINT createFlags = 0;
#ifdef _DEBUG
        createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        D3D_FEATURE_LEVEL achievedLevel;
        hr = D3D11CreateDevice(
            pDXGIAdapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            createFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &pd3dDevice,
            &achievedLevel,
            &pd3dContext
        );

        if (FAILED(hr)) {
            diagLen += snprintf(diagBuf + diagLen, sizeof(diagBuf) - diagLen,
                "D3D11失败(0x%08X) ", (unsigned)hr);
            pDXGIAdapter->Release();
            pDXGIAdapter = nullptr;
            continue;
        }

        diagLen += snprintf(diagBuf + diagLen, sizeof(diagBuf) - diagLen,
            "D3D11_OK(FL=%d.%d) ", (int)achievedLevel >> 12, ((int)achievedLevel >> 8) & 0xF);

        // 枚举该适配器的输出
        int outputCount = 0;
        IDXGIOutput* tempOutput = nullptr;
        for (UINT j = 0; pDXGIAdapter->EnumOutputs(j, &tempOutput) != DXGI_ERROR_NOT_FOUND; ++j) {
            outputCount++;
            DXGI_OUTPUT_DESC outputDesc;
            if (SUCCEEDED(tempOutput->GetDesc(&outputDesc))) {
                int outW = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
                int outH = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
                diagLen += snprintf(diagBuf + diagLen, sizeof(diagBuf) - diagLen,
                    "[输出%u] %dx%d AttachedToDesktop=%d ",
                    j, outW, outH, (int)outputDesc.AttachedToDesktop);
                if (outputDesc.AttachedToDesktop) {
                    pDXGIOutput = tempOutput;
                    screenWidth = outW;
                    screenHeight = outH;
                    // 不 break, 继续遍历以收集诊断信息
                }
            }
            if (!pDXGIOutput || tempOutput != pDXGIOutput) {
                tempOutput->Release();
            }
        }
        if (outputCount == 0) {
            diagLen += snprintf(diagBuf + diagLen, sizeof(diagBuf) - diagLen,
                "无输出 ");
        }

        if (pDXGIOutput) {
            OutputDebugStringA(diagBuf);
            break;  // 找到了挂载到桌面的输出
        }

        diagLen += snprintf(diagBuf + diagLen, sizeof(diagBuf) - diagLen,
            "(无桌面输出,跳过) ");
        pd3dDevice->Release();
        pd3dDevice = nullptr;
        pd3dContext->Release();
        pd3dContext = nullptr;
        pDXGIAdapter->Release();
        pDXGIAdapter = nullptr;
    }

    if (!pd3dDevice || !pDXGIOutput) {
        snprintf(lastError, sizeof(lastError),
            "未找到挂载到桌面的D3D11适配器/输出. 诊断: %s", diagBuf);
        OutputDebugStringA(lastError);
        CleanupDXGI();
        return false;
    }

    IDXGIOutput1* pDXGIOutput1 = nullptr;
    hr = pDXGIOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&pDXGIOutput1);
    if (FAILED(hr)) {
        snprintf(lastError, sizeof(lastError),
            "QueryInterface(IDXGIOutput1) 失败 HRESULT=0x%08X (驱动不支持 DXGI 1.2)", (unsigned)hr);
        OutputDebugStringA(lastError);
        CleanupDXGI();
        return false;
    }

    hr = pDXGIOutput1->DuplicateOutput(pd3dDevice, &pDeskDupl);
    pDXGIOutput1->Release();

    if (FAILED(hr)) {
        const char* reason = "未知错误";
        if (hr == DXGI_ERROR_UNSUPPORTED)
            reason = "DXGI_ERROR_UNSUPPORTED — 桌面模式不兼容(RDP/虚拟机/未装显卡驱动/Win7)";
        else if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
            reason = "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE — 资源被占用(OBS/Discord等正在录屏,或进程残留)";
        else if (hr == E_ACCESSDENIED)
            reason = "E_ACCESSDENIED — 权限不足(需管理员权限或UAC安全桌面冲突)";
        else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            reason = "DXGI_ERROR_DEVICE_REMOVED/RESET — 显卡驱动崩溃或设备被移除";

        snprintf(lastError, sizeof(lastError),
            "DuplicateOutput 失败 HRESULT=0x%08X: %s", (unsigned)hr, reason);
        OutputDebugStringA(lastError);
        CleanupDXGI();
        return false;
    }

    initialized = true;
    return true;
}

void DXScreenCapture::CleanupDXGI() {
    CleanupTextureCache();

    if (frameTexture) {
        frameTexture->Release();
        frameTexture = nullptr;
    }

    if (pDeskDupl) {
        pDeskDupl->Release();
        pDeskDupl = nullptr;
    }
    if (pDXGIOutput) {
        pDXGIOutput->Release();
        pDXGIOutput = nullptr;
    }
    if (pDXGIAdapter) {
        pDXGIAdapter->Release();
        pDXGIAdapter = nullptr;
    }
    if (pd3dContext) {
        pd3dContext->Release();
        pd3dContext = nullptr;
    }
    if (pd3dDevice) {
        pd3dDevice->Release();
        pd3dDevice = nullptr;
    }
    if (pDXGIFactory) {
        pDXGIFactory->Release();
        pDXGIFactory = nullptr;
    }

    initialized = false;
}

bool DXScreenCapture::InitializeTextures() {
    if (textureInitialized) {
        CleanupTextureCache();
        textureInitialized = false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = captureWidth;
    stagingDesc.Height = captureHeight;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = pd3dDevice->CreateTexture2D(&stagingDesc, nullptr, &texturePair_general.stagingTexture);
    if (FAILED(hr) || !texturePair_general.stagingTexture) {
        CleanupTextureCache();
        return false;
    }

    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_EVENT;
    queryDesc.MiscFlags = 0;

    hr = pd3dDevice->CreateQuery(&queryDesc, &texturePair_general.syncQuery);
    if (FAILED(hr)) {
        CleanupTextureCache();
        return false;
    }

    texturePair_general.width = captureWidth;
    texturePair_general.height = captureHeight;
    texturePair_general.inUse = false;

    textureInitialized = true;
    return true;
}

void DXScreenCapture::CleanupTextureCache() {
    if (texturePair_general.isMapped && texturePair_general.stagingTexture && pd3dContext) {
        pd3dContext->Unmap(texturePair_general.stagingTexture, 0);
        texturePair_general.isMapped = false;
    }

    if (texturePair_general.stagingTexture) {
        texturePair_general.stagingTexture->Release();
        texturePair_general.stagingTexture = nullptr;
    }

    if (texturePair_general.syncQuery) {
        texturePair_general.syncQuery->Release();
        texturePair_general.syncQuery = nullptr;
    }

    textureInitialized = false;
}

CachedTexturePair* DXScreenCapture::GetCachedTexturePair(UINT width, UINT height) {
    if (textureInitialized &&
        texturePair_general.stagingTexture &&
        texturePair_general.width == width &&
        texturePair_general.height == height &&
        !texturePair_general.inUse) {

        texturePair_general.inUse = true;
        return &texturePair_general;
    }

    return nullptr;
}

void DXScreenCapture::RecalculateCenterPosition() {
    int centerX = (screenWidth - captureWidth) / 2;
    int centerY = (screenHeight - captureHeight) / 2;

    captureX = std::max(0, centerX);
    captureY = std::max(0, centerY);

    if (captureX + captureWidth > screenWidth) {
        captureWidth = screenWidth - captureX;
    }
    if (captureY + captureHeight > screenHeight) {
        captureHeight = screenHeight - captureY;
    }
}

bool DXScreenCapture::OnOutputChange() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (pDeskDupl) {
        pDeskDupl->Release();
        pDeskDupl = nullptr;
    }

    CleanupTextureCache();

    if (frameTexture) {
        frameTexture->Release();
        frameTexture = nullptr;
    }

    return ReinitializeDuplication();
}

bool DXScreenCapture::ReinitializeDuplication() {
    const int maxRetries = 5;
    int retryCount = 0;

    while (retryCount < maxRetries) {
        if (pDXGIOutput) {
            DXGI_OUTPUT_DESC outputDesc;
            HRESULT hr = pDXGIOutput->GetDesc(&outputDesc);
            if (SUCCEEDED(hr)) {
                screenWidth = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
                screenHeight = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
            }
        }

        if (useCenterMode) {
            RecalculateCenterPosition();
        }

        IDXGIOutput1* pDXGIOutput1 = nullptr;
        HRESULT hr = pDXGIOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&pDXGIOutput1);
        if (FAILED(hr)) {
            retryCount++;
            std::this_thread::sleep_for(std::chrono::milliseconds(200 * retryCount));
            continue;
        }

        hr = pDXGIOutput1->DuplicateOutput(pd3dDevice, &pDeskDupl);
        pDXGIOutput1->Release();

        if (SUCCEEDED(hr)) {
            if (!InitializeTextures()) {
                pDeskDupl->Release();
                pDeskDupl = nullptr;
                retryCount++;
                std::this_thread::sleep_for(std::chrono::milliseconds(200 * retryCount));
                continue;
            }

            reinitCount++;
            return true;
        }

        retryCount++;
        std::this_thread::sleep_for(std::chrono::milliseconds(200 * retryCount));
    }

    return false;
}

bool DXScreenCapture::UpdateFrame(UINT timeoutMs) {
    if (!pDeskDupl) return false;

    IDXGIResource* pDesktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    // �����ȴ� DWM �ϳ�������Ϸ Present ֮���ѣ��¼�������CPU ����Ϊ 0��
    HRESULT hr = pDeskDupl->AcquireNextFrame(timeoutMs, &frameInfo, &pDesktopResource);

    switch (hr) {
    case S_OK:
        break;
    case DXGI_ERROR_WAIT_TIMEOUT:
        // ���û����֡�����ߵ��÷��������Σ������ٷ��ؾ� frameTexture��
        updated = false;
        return true;
    case DXGI_ERROR_ACCESS_LOST:
    case DXGI_ERROR_DEVICE_REMOVED:
        if (OnOutputChange()) {
            updated = false;
            return true;
        }
        return false;
    default:
        return false;
    }

    // ���� AccumulatedFrames �Ƿ� > 0��ֻҪ AcquireNextFrame �ɹ����õ���һ�� desktop image��
    // AccumulatedFrames=0 Ҳ�����ǺϷ��ġ�������¡���������� / dirty rect ֪ͨ����
    // �ܾ����ǻ��� Capture ����ƾ�ն�� 1-2 ֡����������һ��ȡ�� texture��
    if (frameTexture) {
        frameTexture->Release();
        frameTexture = nullptr;
    }

    hr = pDesktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frameTexture);
    pDesktopResource->Release();

    if (SUCCEEDED(hr)) {
        updated = true;
        return true;
    }

    pDeskDupl->ReleaseFrame();
    return true;
}

void DXScreenCapture::ReleaseFrame() {
    if (pDeskDupl) {
        pDeskDupl->ReleaseFrame();
    }
}

void DXScreenCapture::ConvertBGRA2BGR_SIMD(const uint8_t* src, uint8_t* dest, int width, int height, int srcPitch) {
    if (sse42Supported && width >= 4) {
        for (int y = 0; y < height; y++) {
            const uint8_t* srcRow = src + y * srcPitch;
            uint8_t* destRow = dest + y * width * 3;

            if (y + 1 < height) {
                _mm_prefetch((const char*)(src + (y + 1) * srcPitch), _MM_HINT_T0);
            }

            int pixelX = 0;
            for (; pixelX <= width - 4; pixelX += 4) {
                __m128i bgra = _mm_loadu_si128((__m128i*)(srcRow + pixelX * 4));
                __m128i bgr = _mm_shuffle_epi8(bgra,
                    _mm_setr_epi8(
                        0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14,
                        0, 0, 0, 0
                    ));
                _mm_storeu_si128((__m128i*)(destRow + pixelX * 3), bgr);
            }

            for (; pixelX < width; pixelX++) {
                destRow[pixelX * 3 + 0] = srcRow[pixelX * 4 + 0];
                destRow[pixelX * 3 + 1] = srcRow[pixelX * 4 + 1];
                destRow[pixelX * 3 + 2] = srcRow[pixelX * 4 + 2];
            }
        }
    }
    else {
        for (int y = 0; y < height; y++) {
            const uint8_t* srcRow = src + y * srcPitch;
            uint8_t* destRow = dest + y * width * 3;

            for (int pixelX = 0; pixelX < width; pixelX++) {
                destRow[pixelX * 3 + 0] = srcRow[pixelX * 4 + 0];
                destRow[pixelX * 3 + 1] = srcRow[pixelX * 4 + 1];
                destRow[pixelX * 3 + 2] = srcRow[pixelX * 4 + 2];
            }
        }
    }
}

bool DXScreenCapture::ConvertToBMP_Fast(const BYTE* srcData, int width, int height, int srcPitch, std::vector<uint8_t>& outBuffer) {
    if (!ValidateBMPParameters(srcData, width, height, srcPitch)) {
        return false;
    }

    BMPHeaderCache* headerCache = nullptr;
    {
        auto key = std::make_pair(width, height);
        auto it = bmpHeaderCache.find(key);
        if (it == bmpHeaderCache.end()) {
            bmpHeaderCache[key].Initialize(width, height);
        }
        headerCache = &bmpHeaderCache[key];
    }

    int totalSize = headerCache->totalSize;
    int rowSize = headerCache->rowSize;

    // ֱ�ӵ������ buffer ��С
    outBuffer.resize(totalSize);

    // д��ͷ��
    memcpy(outBuffer.data(), &headerCache->fileHeader, sizeof(BMPFileHeader));
    memcpy(outBuffer.data() + sizeof(BMPFileHeader), &headerCache->infoHeader, sizeof(BMPInfoHeader));

    BYTE* destData = outBuffer.data() + sizeof(BMPFileHeader) + sizeof(BMPInfoHeader);

    if (sse42Supported && width >= 4) {
        for (int y = 0; y < height; y++) {
            const BYTE* srcRow = srcData + (height - 1 - y) * srcPitch;
            BYTE* destRow = destData + y * rowSize;

            if (y + 1 < height) {
                _mm_prefetch((const char*)(srcData + (height - 1 - (y + 1)) * srcPitch), _MM_HINT_T0);
            }

            int pixelX = 0;
            for (; pixelX <= width - 4; pixelX += 4) {
                __m128i bgra = _mm_loadu_si128((__m128i*)(srcRow + pixelX * 4));
                __m128i bgr = _mm_shuffle_epi8(bgra,
                    _mm_setr_epi8(
                        0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14,
                        0, 0, 0, 0
                    ));
                _mm_storeu_si128((__m128i*)(destRow + pixelX * 3), bgr);
            }

            for (; pixelX < width; pixelX++) {
                destRow[pixelX * 3 + 0] = srcRow[pixelX * 4 + 0];
                destRow[pixelX * 3 + 1] = srcRow[pixelX * 4 + 1];
                destRow[pixelX * 3 + 2] = srcRow[pixelX * 4 + 2];
            }
        }
    }
    else {
        for (int y = 0; y < height; y++) {
            const BYTE* srcRow = srcData + (height - 1 - y) * srcPitch;
            BYTE* destRow = destData + y * rowSize;

            for (int pixelX = 0; pixelX < width; pixelX++) {
                destRow[pixelX * 3 + 0] = srcRow[pixelX * 4 + 0];
                destRow[pixelX * 3 + 1] = srcRow[pixelX * 4 + 1];
                destRow[pixelX * 3 + 2] = srcRow[pixelX * 4 + 2];
            }
        }
    }

    return true;
}

bool DXScreenCapture::Initialize(int width, int height) {
    if (!InitializeDXGI()) {
        return false;
    }

    int centerX = (screenWidth - width) / 2;
    int centerY = (screenHeight - height) / 2;

    centerX = std::max(0, centerX);
    centerY = std::max(0, centerY);

    useCenterMode = true;

    bool result = InitializeRegion(centerX, centerY, width, height);

    useCenterMode = true;

    return result;
}

bool DXScreenCapture::InitializeRegion(int x, int y, int width, int height) {
    Release();

    if (!InitializeDXGI()) {
        return false;
    }

    useCenterMode = false;

    captureX = std::max(0, std::min(x, screenWidth));
    captureY = std::max(0, std::min(y, screenHeight));
    captureWidth = std::max(1, std::min(width, screenWidth - captureX));
    captureHeight = std::max(1, std::min(height, screenHeight - captureY));

    bool isNewResolution = true;
    for (auto& rc : resolutionFirstCalls) {
        if (rc.width == captureWidth && rc.height == captureHeight) {
            isNewResolution = false;
            break;
        }
    }
    if (isNewResolution) {
        resolutionFirstCalls.emplace_back(captureWidth, captureHeight);
    }

    if (!InitializeTextures()) {
        return false;
    }

    outWidth = captureWidth;
    outHeight = captureHeight;
    outChannels = 3;

    return true;
}

bool DXScreenCapture::SetRegion(int x, int y, int width, int height) {
    return InitializeRegion(x, y, width, height);
}

std::vector<uint8_t> DXScreenCapture::CaptureBGR() {
    std::vector<uint8_t> result;

    if (!initialized || !pd3dDevice || !pd3dContext || !pDeskDupl) {
        return result;
    }

    if (IsResolutionFirstCall(captureWidth, captureHeight)) {
        if (!initialized || !pd3dDevice || !pd3dContext || !pDeskDupl) {
            return result;
        }
    }

    // ������һ֡������æ��ѯ���������ޣ�
    if (!UpdateFrame() || !updated || !frameTexture) {
        return result;
    }

    // ��ȡ������
    CachedTexturePair* pTexturePair = GetCachedTexturePair(captureWidth, captureHeight);
    if (!pTexturePair || !pTexturePair->stagingTexture) {
        if (pTexturePair) pTexturePair->inUse = false;
        return result;
    }

    // ���岶������
    D3D11_BOX sourceBox = {
        (UINT)captureX, (UINT)captureY, 0,
        (UINT)(captureX + captureWidth), (UINT)(captureY + captureHeight), 1
    };

    // GPU��������
    pd3dContext->CopySubresourceRegion(
        pTexturePair->stagingTexture, 0, 0, 0, 0,
        frameTexture, 0, &sourceBox
    );

    // ���GPUͬ����
    pd3dContext->End(pTexturePair->syncQuery);

    // �����ͷ�Desktop Duplication֡��
    ReleaseFrame();

    // �ȴ�GPU��ɿ���
    BOOL queryData = FALSE;
    while (pd3dContext->GetData(pTexturePair->syncQuery, &queryData, sizeof(BOOL), 0) == S_FALSE) {
        std::this_thread::yield();
    }

    // ����Map��ȷ����ȡ��������
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    if (pTexturePair->isMapped) {
        pd3dContext->Unmap(pTexturePair->stagingTexture, 0);
        pTexturePair->isMapped = false;
    }

    HRESULT hr = pd3dContext->Map(
        pTexturePair->stagingTexture, 0,
        D3D11_MAP_READ,
        0,
        &mappedResource
    );

    if (FAILED(hr)) {
        pTexturePair->inUse = false;
        return result;
    }

    pTexturePair->isMapped = true;
    pTexturePair->mappedResource = mappedResource;

    // ׼����� buffer
    size_t bgrSize = captureWidth * captureHeight * 3;
    result.resize(bgrSize);

    // SIMDת�� BGRA -> BGR
    BYTE* srcData = (BYTE*)mappedResource.pData;
    ConvertBGRA2BGR_SIMD(
        srcData,
        result.data(),
        captureWidth,
        captureHeight,
        mappedResource.RowPitch
    );

    // �ͷ�����ռ�ñ�־
    pTexturePair->inUse = false;

    return result;
}

bool DXScreenCapture::CaptureBGR(uint8_t* outBuffer, size_t bufferSize) {
    if (!outBuffer || !initialized || !pd3dDevice || !pd3dContext || !pDeskDupl) {
        return false;
    }

    size_t requiredSize = (size_t)captureWidth * captureHeight * 3;
    if (bufferSize < requiredSize) return false;

    if (IsResolutionFirstCall(captureWidth, captureHeight)) {
        if (!initialized || !pd3dDevice || !pd3dContext || !pDeskDupl) {
            return false;
        }
    }

    // ������һ֡����ʱ������ false���� CaptureLoop �����Ƿ��þ� buffer
    if (!UpdateFrame() || !updated || !frameTexture) {
        return false;
    }

    CachedTexturePair* pTexturePair = GetCachedTexturePair(captureWidth, captureHeight);
    if (!pTexturePair || !pTexturePair->stagingTexture) {
        if (pTexturePair) pTexturePair->inUse = false;
        return false;
    }

    D3D11_BOX sourceBox = {
        (UINT)captureX, (UINT)captureY, 0,
        (UINT)(captureX + captureWidth), (UINT)(captureY + captureHeight), 1
    };

    pd3dContext->CopySubresourceRegion(
        pTexturePair->stagingTexture, 0, 0, 0, 0,
        frameTexture, 0, &sourceBox
    );

    pd3dContext->End(pTexturePair->syncQuery);
    ReleaseFrame();

    BOOL queryData = FALSE;
    while (pd3dContext->GetData(pTexturePair->syncQuery, &queryData, sizeof(BOOL), 0) == S_FALSE) {
        std::this_thread::yield();
    }

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    if (pTexturePair->isMapped) {
        pd3dContext->Unmap(pTexturePair->stagingTexture, 0);
        pTexturePair->isMapped = false;
    }

    HRESULT hr = pd3dContext->Map(
        pTexturePair->stagingTexture, 0,
        D3D11_MAP_READ, 0, &mappedResource
    );

    if (FAILED(hr)) {
        pTexturePair->inUse = false;
        return false;
    }

    pTexturePair->isMapped = true;
    pTexturePair->mappedResource = mappedResource;

    ConvertBGRA2BGR_SIMD(
        (const uint8_t*)mappedResource.pData,
        outBuffer,
        captureWidth, captureHeight,
        mappedResource.RowPitch
    );

    pTexturePair->inUse = false;
    return true;
}

std::vector<uint8_t> DXScreenCapture::CaptureBMP() {
    std::vector<uint8_t> result;

    if (!initialized || !pd3dDevice || !pd3dContext || !pDeskDupl) {
        return result;
    }

    if (IsResolutionFirstCall(captureWidth, captureHeight)) {
        if (!initialized || !pd3dDevice || !pd3dContext || !pDeskDupl) {
            return result;
        }
    }

    // ������һ֡���������ޣ�
    if (!UpdateFrame() || !updated || !frameTexture) {
        return result;
    }

    // ��ȡ������
    CachedTexturePair* pTexturePair = GetCachedTexturePair(captureWidth, captureHeight);
    if (!pTexturePair || !pTexturePair->stagingTexture) {
        if (pTexturePair) pTexturePair->inUse = false;
        return result;
    }

    // ���岶������
    D3D11_BOX sourceBox = {
        (UINT)captureX, (UINT)captureY, 0,
        (UINT)(captureX + captureWidth), (UINT)(captureY + captureHeight), 1
    };

    // GPU��������
    pd3dContext->CopySubresourceRegion(
        pTexturePair->stagingTexture, 0, 0, 0, 0,
        frameTexture, 0, &sourceBox
    );

    // ���GPUͬ����
    pd3dContext->End(pTexturePair->syncQuery);

    // �����ͷ�Desktop Duplication֡��
    ReleaseFrame();

    // �ȴ�GPU��ɿ���
    BOOL queryData = FALSE;
    while (pd3dContext->GetData(pTexturePair->syncQuery, &queryData, sizeof(BOOL), 0) == S_FALSE) {
        std::this_thread::yield();
    }

    // ����Map��ȷ����ȡ��������
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    if (pTexturePair->isMapped) {
        pd3dContext->Unmap(pTexturePair->stagingTexture, 0);
        pTexturePair->isMapped = false;
    }

    HRESULT hr = pd3dContext->Map(
        pTexturePair->stagingTexture, 0,
        D3D11_MAP_READ,
        0,
        &mappedResource
    );

    if (FAILED(hr)) {
        pTexturePair->inUse = false;
        return result;
    }

    pTexturePair->isMapped = true;
    pTexturePair->mappedResource = mappedResource;

    // ת��ΪBMP
    BYTE* srcData = (BYTE*)mappedResource.pData;
    bool success = ConvertToBMP_Fast(srcData, captureWidth, captureHeight, mappedResource.RowPitch, result);

    // �ͷ�����ռ�ñ�־
    pTexturePair->inUse = false;

    if (!success) {
        result.clear();
    }

    return result;
}

void DXScreenCapture::Release() {
    CleanupDXGI();
    bmpHeaderCache.clear();
    resolutionFirstCalls.clear();
    frameCounter = 0;
    updated = false;
}
