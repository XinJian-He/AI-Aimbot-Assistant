#ifndef DXSCREEN_CAPTURE_H
#define DXSCREEN_CAPTURE_H

#include "IScreenCapture.h"
#include <immintrin.h>
#include <atomic>

class DXScreenCapture {
public:
    DXScreenCapture();
    ~DXScreenCapture();

    // �����ӿ�
    bool Initialize(int width, int height);
    bool InitializeRegion(int x, int y, int width, int height);
    bool SetRegion(int x, int y, int width, int height);

    std::vector<uint8_t> CaptureBGR();
    bool CaptureBGR(uint8_t* outBuffer, size_t bufferSize);
    std::vector<uint8_t> CaptureBMP();

    void Release();

    // ��ȡ��Ϣ
    int GetWidth() const { return outWidth; }
    int GetHeight() const { return outHeight; }
    int GetChannels() const { return outChannels; }
    const char* GetLastError() const { return lastError; }

private:
    // DXGI���
    bool InitializeDXGI();
    void CleanupDXGI();
    bool InitializeTextures();
    void CleanupTextureCache();
    bool UpdateFrame(UINT timeoutMs = INFINITE);  // DXGI ��������һ֡��Ĭ�� INFINITE = ��ȫ������Ϸ��CPU��0��
    void ReleaseFrame();

    // ����ָ�
    bool OnOutputChange();
    bool ReinitializeDuplication();
    void RecalculateCenterPosition();

    // ��������
    CachedTexturePair* GetCachedTexturePair(UINT width, UINT height);

    // ��ʽת�� - ��Ϊֱ����䵽 vector
    bool ConvertToBMP_Fast(const BYTE* srcData, int width, int height, int srcPitch, std::vector<uint8_t>& outBuffer);
    void ConvertBGRA2BGR_SIMD(const uint8_t* bgra, uint8_t* bgr, int width, int height, int stride);

    // ��������
    bool IsSSE42Supported();
    inline bool ValidateBMPParameters(const BYTE* srcData, int width, int height, int srcPitch);
    inline bool IsResolutionFirstCall(int width, int height);

    // DXGI����
    ID3D11Device* pd3dDevice;
    ID3D11DeviceContext* pd3dContext;
    IDXGIOutputDuplication* pDeskDupl;
    IDXGIOutput* pDXGIOutput;
    IDXGIAdapter1* pDXGIAdapter;
    IDXGIFactory1* pDXGIFactory;
    ID3D11Texture2D* frameTexture;

    // ��������
    int captureX;
    int captureY;
    int captureWidth;
    int captureHeight;
    int outWidth;
    int outHeight;
    int outChannels;

    // ��������
    CachedTexturePair texturePair_general;
    CachedTexturePair* currentTexturePair;

    // ״̬��־
    bool textureInitialized;
    bool initialized;
    bool updated;
    bool sse42Supported;
    bool useCenterMode;

    // ��Ļ��Ϣ
    int screenWidth;
    int screenHeight;

    // ����
    std::unordered_map<std::pair<int, int>, BMPHeaderCache, PairHash> bmpHeaderCache;
    std::vector<ResolutionFirstCall> resolutionFirstCalls;

    // ͳ��
    int frameCounter;
    std::atomic<int> reinitCount;

    // ������Ϣ
    mutable char lastError[512] = {0};
};

#endif // DXSCREEN_CAPTURE_H
