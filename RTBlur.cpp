// Basic layout for a DirectX 11 + ImGui application with Gaussian blur capability
// This is the skeleton; you’ll need to fill in full image loading, shader compilation, etc.

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imconfig.h"
#include "imgui_internal.h"
#include <vector>
#include <string>
#include <shobjidl.h> // For IFileOpenDialog


using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "D3DCompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "Ole32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ReinitDeviceAndPipeline(HWND hWnd);
void CreateTempRenderTarget(UINT x, UINT y);
void CreateBlurSettingsBuffer();
void LoadBlurShader();
void CreateBlurRenderTarget(UINT width, UINT height);

// Globals
HWND hwnd = nullptr;
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

float g_blurRadius = 5.0f;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


ID3D11ShaderResourceView* LoadTextureFromFile(const wchar_t* filename) {
    // Initialize COM
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IWICImagingFactory> wicFactory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr)) return nullptr;

    hr = wicFactory->CreateDecoderFromFilename(filename, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) return nullptr;

    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return nullptr;

    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return nullptr;

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return nullptr;

    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);

    std::vector<BYTE> imageData(width * height * 4);
    hr = converter->CopyPixels(nullptr, width * 4, (UINT)imageData.size(), imageData.data());
    if (FAILED(hr)) return nullptr;

    // Create the texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = imageData.data();
    initData.SysMemPitch = width * 4;

    ComPtr<ID3D11Texture2D> texture;
    hr = g_pd3dDevice->CreateTexture2D(&texDesc, &initData, &texture);
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = g_pd3dDevice->CreateShaderResourceView(texture.Get(), &srvDesc, &srv);
    if (FAILED(hr)) return nullptr;

    return srv;
}

std::wstring OpenFileDialog() {
    ComPtr<IFileOpenDialog> fileDialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&fileDialog));
    if (FAILED(hr)) return L"";

    // File types filter (images)
    COMDLG_FILTERSPEC fileTypes[] = {
        { L"Image Files", L"*.jpg;*.jpeg;*.png;*.bmp;*.tiff" },
        { L"All Files", L"*.*" }
    };

    fileDialog->SetFileTypes(_countof(fileTypes), fileTypes);
    fileDialog->SetTitle(L"Select an image");

    hr = fileDialog->Show(nullptr);
    if (FAILED(hr)) return L"";

    ComPtr<IShellItem> item;
    hr = fileDialog->GetResult(&item);
    if (FAILED(hr)) return L"";

    PWSTR filePath = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
    if (FAILED(hr)) return L"";

    std::wstring result(filePath);
    CoTaskMemFree(filePath);
    return result;
}
// Suppose you already do something like this in LoadBlurShader(). 
// Now let's do a separate one for the fullscreen pass:

ComPtr<ID3D11VertexShader> g_fullscreenVS = nullptr;
ComPtr<ID3D11PixelShader>  g_fullscreenPS = nullptr;
ComPtr<ID3D11InputLayout>  g_inputLayout = nullptr;

void LoadFullscreenShaders()
{
    ComPtr<ID3DBlob> vsBlob, psBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompileFromFile(
        L"FullScreenPass.hlsl",
        nullptr, nullptr,
        "VSMain", // Vertex shader entry
        "vs_5_0", 0, 0, &vsBlob, &errorBlob
    );
    if (FAILED(hr))
    {
        if (errorBlob) {
            OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        }
        return;
    }

    hr = g_pd3dDevice->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, &g_fullscreenVS
    );

    // Now compile pixel shader
    hr = D3DCompileFromFile(
        L"FullScreenPass.hlsl",
        nullptr, nullptr,
        "PSMain", // Pixel shader entry
        "ps_5_0", 0, 0, &psBlob, &errorBlob
    );
    if (FAILED(hr))
    {
        if (errorBlob) {
            OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        }
        return;
    }

    hr = g_pd3dDevice->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, &g_fullscreenPS
    );

    // Create input layout (matches VSInput)
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    hr = g_pd3dDevice->CreateInputLayout(
        layoutDesc, _countof(layoutDesc),
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        &g_inputLayout
    );
}

ComPtr<ID3D11Buffer> g_fullscreenVB;

struct FullscreenVertex
{
    float x, y; // position in clip space
    float u, v; // uv coords
};

void CreateFullscreenTriangle()
{
    // Three vertices that cover the screen
    // (Using bottom-left, top-left out of bounds, bottom-right out of bounds)
    FullscreenVertex verts[3] =
    {
        { -1.0f, -1.0f,  0.0f, 1.0f }, // bottom-left
        { -1.0f,  3.0f,  0.0f, -1.0f}, // top-left (go way out of screen)
        {  3.0f, -1.0f,  2.0f, 1.0f }, // bottom-right
    };

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(verts);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = verts;

    HRESULT hr = g_pd3dDevice->CreateBuffer(&bd, &initData, &g_fullscreenVB);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create fullscreen VB.\n");
    }
}

ComPtr<ID3D11SamplerState> g_linearClampSampler;

void CreateSamplerState()
{
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = g_pd3dDevice->CreateSamplerState(&sampDesc, &g_linearClampSampler);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create sampler state.\n");
    }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static int selectedAdapterIndex = 0;

D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
D3D_FEATURE_LEVEL outFeatureLevel;
std::vector<std::wstring> adapterNames;
std::vector<ComPtr<IDXGIAdapter>> adapters;

void EnumAllAdapters() {
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);

    adapters.clear();
    adapterNames.clear();

    ComPtr<IDXGIAdapter> adapter;
    UINT index = 0;
    while (factory->EnumAdapters(index, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        adapters.push_back(adapter);

        // Convert desc.Description (WCHAR array) to std::wstring
        adapterNames.push_back(std::wstring(desc.Description));

        adapter.Reset();
        index++;
    }
}


DXGI_SWAP_CHAIN_DESC sd = {};


void InitD3D(HWND hWnd) {

    sd.BufferCount = 2;
    sd.BufferDesc.Width = 1280;
    sd.BufferDesc.Height = 720;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    D3D11CreateDeviceAndSwapChain(adapters[selectedAdapterIndex].Get(), D3D_DRIVER_TYPE_UNKNOWN,
        nullptr, 0, featureLevels, _countof(featureLevels), D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &outFeatureLevel, &g_pd3dDeviceContext);

    LoadFullscreenShaders();    // <-- compile VSMain/PSMain from FullScreenPass.hlsl
    CreateFullscreenTriangle(); // <-- create the big triangle
    CreateSamplerState();       // <-- create a sampler



    LoadBlurShader();
    CreateRenderTarget();
    CreateBlurRenderTarget(1280, 720);

    CreateBlurSettingsBuffer();

}

void CleanupD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

// Pseudocode
void ReinitDeviceAndPipeline(HWND hWnd)
{
    ImGui_ImplDX11_Shutdown();
    // If there’s an existing device, release everything
    CleanupD3D();

    InitD3D(hWnd);

    // CreateRenderTarget();
    // CreateBlurRenderTarget(...);
    // CreateTempRenderTarget(...);
    // CreateBlurSettingsBuffer();
    // (Re)Load Shaders
    // (Re)Load any needed textures
    CreateBlurRenderTarget(1280, 720);
    CreateTempRenderTarget(1280, 720);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Re-init ImGui
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
}


void ShowAdapterPicker() {
    ImGui::Text("Pick GPU:");
    if (ImGui::BeginCombo("GPU List", (char*)adapterNames[selectedAdapterIndex].c_str())) {
        for (int i = 0; i < adapterNames.size(); i++) {
            bool isSelected = (selectedAdapterIndex == i);
            if (ImGui::Selectable((char*)adapterNames[i].c_str(), isSelected)) {
                selectedAdapterIndex = i;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // Button to confirm selection
    if (ImGui::Button("Use This GPU")) {
        ReinitDeviceAndPipeline(hwnd);
    }
}
struct BlurSettings {
    DirectX::XMFLOAT2 texelSize;
    float blurRadius;
    float padding; // Padding to align to 16 bytes
};

ComPtr<ID3D11Buffer> g_blurSettingsBuffer;

void CreateBlurSettingsBuffer() {
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(BlurSettings);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = g_pd3dDevice->CreateBuffer(&cbDesc, nullptr, &g_blurSettingsBuffer);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create blur settings buffer.\n");
        // Optionally, log the HRESULT value for debugging
        wchar_t errorMsg[256];
        swprintf_s(errorMsg, L"CreateBuffer failed with HRESULT: 0x%08X\n", hr);
        OutputDebugString(errorMsg);
    }
    g_pd3dDevice->CreateBuffer(&cbDesc, nullptr, &g_blurSettingsBuffer);
}

void UpdateBlurSettings(float blurRadius, UINT width, UINT height) {
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    g_pd3dDeviceContext->Map(g_blurSettingsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);

    BlurSettings* settings = reinterpret_cast<BlurSettings*>(mappedResource.pData);
    settings->texelSize = DirectX::XMFLOAT2(1.0f / width, 1.0f / height);
    settings->blurRadius = blurRadius;

    g_pd3dDeviceContext->Unmap(g_blurSettingsBuffer.Get(), 0);
    wchar_t debugMsg[256];
    // swprintf_s(debugMsg, L"Blur Radius: %f\n", blurRadius);
    // OutputDebugString(debugMsg);

}


ComPtr<ID3D11Texture2D> g_blurRenderTargetTexture;
ComPtr<ID3D11RenderTargetView> g_blurRenderTargetView;
ComPtr<ID3D11ShaderResourceView> g_blurShaderResourceView;

void CreateBlurRenderTarget(UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = g_pd3dDevice->CreateTexture2D(&texDesc, nullptr, &g_blurRenderTargetTexture);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create blur render target texture.\n");
    }

    hr = g_pd3dDevice->CreateRenderTargetView(g_blurRenderTargetTexture.Get(), nullptr, &g_blurRenderTargetView);
    if (FAILED(hr)) {
        OutputDebugString(L"Failed to create blur render target view.\n");
    }

    hr = g_pd3dDevice->CreateShaderResourceView(g_blurRenderTargetTexture.Get(), nullptr, &g_blurShaderResourceView);
    if (!g_blurShaderResourceView) {
        OutputDebugString(L"g_blurShaderResourceView is null.\n");
    }
    else {
        OutputDebugString(L"g_blurShaderResourceView created successfully.\n");
    }


}

ComPtr<ID3D11Texture2D> g_tempTexture;
ComPtr<ID3D11RenderTargetView> g_tempRTV;
ComPtr<ID3D11ShaderResourceView> g_tempSRV;

// Create these once in a function, similar to CreateBlurRenderTarget(...)
void CreateTempRenderTarget(UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = g_pd3dDevice->CreateTexture2D(&texDesc, nullptr, &g_tempTexture);
    // check hr...

    hr = g_pd3dDevice->CreateRenderTargetView(g_tempTexture.Get(), nullptr, &g_tempRTV);
    // check hr...

    hr = g_pd3dDevice->CreateShaderResourceView(g_tempTexture.Get(), nullptr, &g_tempSRV);
    // check hr...
}


ComPtr<ID3D11PixelShader> g_blurHorizontalPS;
ComPtr<ID3D11PixelShader> g_blurVerticalPS;

void LoadBlurShader() {
    ComPtr<ID3DBlob> psHorizontalBlob, psVerticalBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompileFromFile(
        L"GaussianBlurShader.hlsl", // Ensure this path is correct
        nullptr, nullptr, "PSHorizontalBlur", "ps_5_0", 0, 0, &psHorizontalBlob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA((char*)errorBlob->GetBufferPointer()); // Log shader compilation errors
        }
        wchar_t errorMsg[256];
        swprintf_s(errorMsg, L"Failed to compile GaussianBlurShader.hlsl. HRESULT: 0x%08X\n", hr);
        OutputDebugString(errorMsg);
        return;
    }

    hr = g_pd3dDevice->CreatePixelShader(psHorizontalBlob->GetBufferPointer(), psHorizontalBlob->GetBufferSize(), nullptr, &g_blurHorizontalPS);
    // Vertical
    hr = D3DCompileFromFile(
        L"GaussianBlurShader.hlsl",
        nullptr, nullptr,
        "PSVerticalBlur", // entry point
        "ps_5_0",
        0, 0,
        &psVerticalBlob,
        &errorBlob
    );
    if (FAILED(hr)) {
        // handle error
    }

    hr = g_pd3dDevice->CreatePixelShader(
        psVerticalBlob->GetBufferPointer(),
        psVerticalBlob->GetBufferSize(),
        nullptr,
        &g_blurVerticalPS
    );
}



void ApplyGaussianBlur(
    ID3D11ShaderResourceView* inputSRV,
    ID3D11RenderTargetView* outputRTV,
    float blurRadius)
{
    // 1) HORIZONTAL PASS --> g_tempRTV
    // Clear the temp RT
    float clearColor[4] = { 0, 0, 0, 1 };
    g_pd3dDeviceContext->ClearRenderTargetView(g_tempRTV.Get(), clearColor);

    // Bind temp RT as output
    g_pd3dDeviceContext->OMSetRenderTargets(1, g_tempRTV.GetAddressOf(), nullptr);

    // We'll use the width/height from the temp texture or the input image
    D3D11_TEXTURE2D_DESC texDesc;
    g_tempTexture->GetDesc(&texDesc); // or the input texture desc
    UpdateBlurSettings(blurRadius, texDesc.Width, texDesc.Height);

    // Bind horizontal blur PS
    g_pd3dDeviceContext->PSSetShader(g_blurHorizontalPS.Get(), nullptr, 0);

    // Samplers, constants, SRV
    g_pd3dDeviceContext->PSSetSamplers(0, 1, g_linearClampSampler.GetAddressOf());
    g_pd3dDeviceContext->PSSetConstantBuffers(0, 1, g_blurSettingsBuffer.GetAddressOf());
    g_pd3dDeviceContext->PSSetShaderResources(0, 1, &inputSRV);

    // Vertex + layout
    g_pd3dDeviceContext->VSSetShader(g_fullscreenVS.Get(), nullptr, 0);
    g_pd3dDeviceContext->IASetInputLayout(g_inputLayout.Get());
    // Bind your fullscreen VB
    UINT stride = sizeof(FullscreenVertex);
    UINT offset = 0;
    ID3D11Buffer* vb = g_fullscreenVB.Get();
    g_pd3dDeviceContext->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_pd3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Set viewport
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (float)texDesc.Width;
    vp.Height = (float)texDesc.Height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    g_pd3dDeviceContext->RSSetViewports(1, &vp);

    // Draw
    g_pd3dDeviceContext->Draw(3, 0);

    // 2) VERTICAL PASS --> outputRTV
    g_pd3dDeviceContext->ClearRenderTargetView(outputRTV, clearColor);
    g_pd3dDeviceContext->OMSetRenderTargets(1, &outputRTV, nullptr);

    // same width/height for vertical pass
    UpdateBlurSettings(blurRadius, texDesc.Width, texDesc.Height);

    // Bind vertical blur PS
    g_pd3dDeviceContext->PSSetShader(g_blurVerticalPS.Get(), nullptr, 0);
    // Samplers, constants
    g_pd3dDeviceContext->PSSetSamplers(0, 1, g_linearClampSampler.GetAddressOf());
    g_pd3dDeviceContext->PSSetConstantBuffers(0, 1, g_blurSettingsBuffer.GetAddressOf());

    // Now the input is the **temp SRV** from pass #1
    ID3D11ShaderResourceView* tempSRVPtr = g_tempSRV.Get();
    g_pd3dDeviceContext->PSSetShaderResources(0, 1, &tempSRVPtr);

    // same geometry + viewport
    g_pd3dDeviceContext->VSSetShader(g_fullscreenVS.Get(), nullptr, 0);
    g_pd3dDeviceContext->IASetInputLayout(g_inputLayout.Get());
    g_pd3dDeviceContext->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_pd3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_pd3dDeviceContext->RSSetViewports(1, &vp);

    // Final draw
    g_pd3dDeviceContext->Draw(3, 0);

    // Done! outputRTV now has the horizontally + vertically blurred image
}


void DrawFullScreenQuad(ID3D11ShaderResourceView* inputSRV)
{
    // 1) Bind the VB
    UINT stride = sizeof(FullscreenVertex);
    UINT offset = 0;
    ID3D11Buffer* vb = g_fullscreenVB.Get();
    g_pd3dDeviceContext->IASetVertexBuffers(0, 1, &vb, &stride, &offset);

    // 2) Set topology
    g_pd3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 3) Input layout + vertex shader + pixel shader
    g_pd3dDeviceContext->IASetInputLayout(g_inputLayout.Get());
    g_pd3dDeviceContext->VSSetShader(g_fullscreenVS.Get(), nullptr, 0);
    g_pd3dDeviceContext->PSSetShader(g_fullscreenPS.Get(), nullptr, 0);

    // 4) Sampler + SRV
    ID3D11ShaderResourceView* srvs[1] = { inputSRV };
    g_pd3dDeviceContext->PSSetShaderResources(0, 1, srvs);
    g_pd3dDeviceContext->PSSetSamplers(0, 1, g_linearClampSampler.GetAddressOf());

    // 5) Now we can draw
    g_pd3dDeviceContext->Draw(3, 0);

    // 6) Unbind SRV from PS slot if needed
    srvs[0] = nullptr;
    g_pd3dDeviceContext->PSSetShaderResources(0, 1, srvs);
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(NULL), NULL, NULL, NULL, NULL,
        L"DX11App", NULL };
    RegisterClassEx(&wc);
    hwnd = CreateWindow(wc.lpszClassName, L"DirectX 11 Gaussian Blur App",
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720,
        NULL, NULL, wc.hInstance, NULL);

    EnumAllAdapters();
    selectedAdapterIndex = 0;
    InitD3D(hwnd);
	CreateBlurRenderTarget(1280, 720);
	CreateTempRenderTarget(1280, 720);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

   


    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        static float oldBlurRadius = 0.0f;     // track last blur slider value
        static bool needsUpdate = false;       // do we need to re-blur?

        // check if slider changed
        if (fabsf(g_blurRadius - oldBlurRadius) > 0.0001f) {
            oldBlurRadius = g_blurRadius;
            needsUpdate = true;
        }
        
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("GPU Selection:");
        ShowAdapterPicker();
        ImGui::End();

        static ID3D11ShaderResourceView* g_loadedImageSRV = nullptr;

        if (ImGui::Button("Open Image")) {
            // load image
            needsUpdate = true; // new image => re-blur

            std::wstring filePath = OpenFileDialog();
            if (!filePath.empty()) {
                if (g_loadedImageSRV) g_loadedImageSRV->Release(); // Free old texture
                g_loadedImageSRV = LoadTextureFromFile(filePath.c_str());
                if (!g_loadedImageSRV) {
                    OutputDebugString(L"Failed to load image. Check the file path and format.\n");
                }
                else {
                    // Retrieve the dimensions of the loaded texture
                    ComPtr<ID3D11Resource> resource;
                    g_loadedImageSRV->GetResource(&resource);

                    ComPtr<ID3D11Texture2D> texture;
                    resource.As(&texture);

                    D3D11_TEXTURE2D_DESC texDesc;
                    texture->GetDesc(&texDesc);
                }
            }
        }

        if (g_loadedImageSRV) {
            // Use the actual image dimensions for display size
            static UINT imageWidth = 0, imageHeight = 0;
            // Retrieve the dimensions of the loaded texture
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
            g_loadedImageSRV->GetDesc(&srvDesc);

            ComPtr<ID3D11Resource> resource;
            g_loadedImageSRV->GetResource(&resource);

            ComPtr<ID3D11Texture2D> texture;
            resource.As(&texture);

            D3D11_TEXTURE2D_DESC texDesc;
            texture->GetDesc(&texDesc);

            imageWidth = texDesc.Width;
            imageHeight = texDesc.Height;
            // Get the available size in the ImGui window
            ImVec2 availableSize = ImGui::GetContentRegionAvail();

            // Calculate the aspect ratio of the image
            float aspectRatio = (float)imageWidth / (float)imageHeight;

            // Adjust the size to maintain the aspect ratio
            if (availableSize.x / aspectRatio <= availableSize.y) {
                availableSize.y = availableSize.x / aspectRatio;
            }
            else {
                availableSize.x = availableSize.y * aspectRatio;
            }
            float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // Black

            if (needsUpdate && g_loadedImageSRV) {
                g_pd3dDeviceContext->ClearRenderTargetView(g_blurRenderTargetView.Get(), clearColor);

                ApplyGaussianBlur(g_loadedImageSRV, g_blurRenderTargetView.Get(), g_blurRadius);
                needsUpdate = false;
            }

            ImVec2 avail = ImGui::GetContentRegionAvail();
            // Display the image with correct UV mapping and resolution
            ImGui::Image(reinterpret_cast<ImTextureID>(g_blurShaderResourceView.Get()), availableSize, ImVec2(0, 0), ImVec2(1, 1));
        }
        // GUI
        ImGui::Begin("Gaussian Blur Settings");
        ImGui::SliderFloat("Blur Radius", &g_blurRadius, 0.001f, 120.0f);
        ImGui::Text("Placeholder for image display");
        ImGui::End();

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        float clear_color[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupD3D();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);

    return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();

            // Recreate your blur RT with the new width/height
            CreateBlurRenderTarget(LOWORD(lParam), HIWORD(lParam));

            D3D11_VIEWPORT vp = {};
            vp.TopLeftX = 0.0f;
            vp.TopLeftY = 0.0f;
            vp.Width = static_cast<float>(LOWORD(lParam));  // new width
            vp.Height = static_cast<float>(HIWORD(lParam));  // new height
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            g_pd3dDeviceContext->RSSetViewports(1, &vp);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
