#pragma once
#include <d3d11.h>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

// Decode the original transparent PNG from the executable, independent of cwd.
inline HRESULT LoadHumanTexture(ID3D11Device *device, ID3D11ShaderResourceView **output, HMODULE module, int resourceId = 101)
{
    using Microsoft::WRL::ComPtr;
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource)
        return HRESULT_FROM_WIN32(GetLastError());
    HGLOBAL data = LoadResource(module, resource);
    auto bytes = static_cast<BYTE *>(LockResource(data));
    DWORD length = SizeofResource(module, resource);
    if (!bytes || !length)
        return E_FAIL;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        return hr;
    ComPtr<IWICStream> stream;
    if (FAILED(hr = factory->CreateStream(&stream)))
        return hr;
    if (FAILED(hr = stream->InitializeFromMemory(bytes, length)))
        return hr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)))
        return hr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(hr = decoder->GetFrame(0, &frame)))
        return hr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(hr = factory->CreateFormatConverter(&converter)))
        return hr;
    if (FAILED(hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                          0, WICBitmapPaletteTypeCustom)))
        return hr;
    UINT width = 0, height = 0;
    if (FAILED(hr = converter->GetSize(&width, &height)))
        return hr;
    if (!width || !height || width > 4096 || height > 4096)
        return E_INVALIDARG;
    std::vector<BYTE> pixels(width * height * 4);
    if (FAILED(hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())))
        return hr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(hr = device->CreateTexture2D(&desc, &initial, &texture)))
        return hr;
    return device->CreateShaderResourceView(texture.Get(), nullptr, output);
}
