#include "Common/d3dApp.h"
#include "Common/MathHelper.h"
#include "Common/UploadBuffer.h"
#include "Common/Camera.h"
#include "Model.h"
#include "RenderingSystem.h"

#include <wincodec.h>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cmath>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

//======================================================================================
// Загрузка PNG/JPG/BMP через WIC (встроено в Windows) + генерация mip-уровней на CPU
//======================================================================================
static IWICImagingFactory* GetWICFactory()
{
    static ComPtr<IWICImagingFactory> factory;
    if (!factory)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            return nullptr;
    }
    return factory.Get();
}

static HRESULT LoadTextureWIC(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    const wchar_t* fileName, ComPtr<ID3D12Resource>& texture, ComPtr<ID3D12Resource>& uploadHeap)
{
    IWICImagingFactory* wic = GetWICFactory();
    if (!wic) return E_FAIL;

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic->CreateDecoderFromFilename(fileName, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;

    UINT width = 0, height = 0;
    frame->GetSize(&width, &height);
    if (width == 0 || height == 0) return E_FAIL;

    ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;

    std::vector<std::vector<uint8_t>> mips(1);
    mips[0].resize(size_t(width) * height * 4);
    hr = converter->CopyPixels(nullptr, width * 4, (UINT)mips[0].size(), mips[0].data());
    if (FAILED(hr)) return hr;

    std::vector<UINT> mipW{ width }, mipH{ height };
    while (mipW.back() > 1 || mipH.back() > 1)
    {
        const UINT sw = mipW.back(), sh = mipH.back();
        const UINT dw = (std::max)(1u, sw / 2), dh = (std::max)(1u, sh / 2);

        std::vector<uint8_t> dst(size_t(dw) * dh * 4);
        const std::vector<uint8_t>& src = mips.back();

        for (UINT y = 0; y < dh; ++y)
        {
            const UINT y0 = (std::min)(2 * y, sh - 1), y1 = (std::min)(2 * y + 1, sh - 1);
            for (UINT x = 0; x < dw; ++x)
            {
                const UINT x0 = (std::min)(2 * x, sw - 1), x1 = (std::min)(2 * x + 1, sw - 1);
                for (UINT c = 0; c < 4; ++c)
                {
                    UINT sum = src[(size_t(y0) * sw + x0) * 4 + c] + src[(size_t(y0) * sw + x1) * 4 + c]
                             + src[(size_t(y1) * sw + x0) * 4 + c] + src[(size_t(y1) * sw + x1) * 4 + c];
                    dst[(size_t(y) * dw + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
                }
            }
        }

        mips.push_back(std::move(dst));
        mipW.push_back(dw);
        mipH.push_back(dh);
    }
    const UINT mipCount = (UINT)mips.size();

    CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, (UINT16)mipCount);
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
    if (FAILED(hr)) return hr;

    const UINT64 uploadSize = GetRequiredIntermediateSize(texture.Get(), 0, mipCount);
    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    hr = device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadHeap));
    if (FAILED(hr)) return hr;

    std::vector<D3D12_SUBRESOURCE_DATA> subresources(mipCount);
    for (UINT i = 0; i < mipCount; ++i)
    {
        subresources[i].pData = mips[i].data();
        subresources[i].RowPitch = LONG_PTR(mipW[i]) * 4;
        subresources[i].SlicePitch = subresources[i].RowPitch * mipH[i];
    }
    UpdateSubresources(cmdList, texture.Get(), uploadHeap.Get(), 0, 0, mipCount, subresources.data());

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);

    return S_OK;
}

//======================================================================================
// Структуры приложения
//======================================================================================
struct ObjectConstants
{
    DirectX::XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
};

struct MaterialAnim
{
    XMFLOAT2 Tiling = { 1.0f, 1.0f };
    XMFLOAT2 ScrollSpeed = { 0.0f, 0.0f };
};

struct MaterialTextures
{
    int Diffuse = 0;
    int Alpha = 0;
};

class SponzaApp : public D3DApp
{
public:
    SponzaApp(HINSTANCE hInstance);
    SponzaApp(const SponzaApp& rhs) = delete;
    SponzaApp& operator=(const SponzaApp& rhs) = delete;
    ~SponzaApp();

    virtual bool Initialize() override;

private:
    virtual void OnResize() override;
    virtual void Update(const GameTimer& gt) override;
    virtual void Draw(const GameTimer& gt) override;
    virtual void OnMouseDown(WPARAM btnState, int x, int y) override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y) override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y) override;

    bool BuildModel();
    void CreateWhiteTexture();
    int  LoadTexture(const std::string& path);
    void BuildTextures();
    void BuildMaterialSrvs();
    void BuildConstantBuffers();
    void BuildMaterialAnimations();
    void BuildLights();

    void UpdateMaterialCB();
    void UpdateLights();
    bool KeyPressed(int vk);   // true один раз на нажатие

private:
    RenderingSystem mRenderer;
    bool mRendererReady = false;

    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;
    std::unique_ptr<UploadBuffer<MaterialConstants>> mMaterialCB = nullptr;

    std::unique_ptr<Model> mSponza = nullptr;
    std::unique_ptr<MeshGeometry> mSponzaGeo = nullptr;

    std::vector<std::unique_ptr<Texture>> mTextures;
    std::unordered_map<std::string, int> mTextureLookup;
    std::vector<MaterialTextures> mMatTextures;
    std::vector<MaterialAnim> mMatAnims;

    XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
    Camera mCamera;

    float mMoveSpeed = 30.0f;
    POINT mLastMousePos = {};

    // Тайлинг/анимация
    float mGlobalTiling = 1.0f;
    bool  mAnimEnabled = true;
    float mAnimTime = 0.0f;

    // Свет
    std::vector<XMFLOAT3> mPointLightBase;   // исходные позиции точечных источников (для анимации)
    size_t mFirstPointLight = 0;
    size_t mSunIndex = 0;
    size_t mFlashlightIndex = 0;
    float  mSceneHeight = 1.0f;
    float  mSunIntensity = 0.6f;
    float  mFlashlightIntensity = 3.0f;
    bool   mSunOn = true;
    bool   mFlashlightOn = true;

    std::unordered_map<int, bool> mKeyWasDown;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance, PSTR cmdLine, int showCmd)
{
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        SponzaApp theApp(hInstance);
        if (!theApp.Initialize())
            return 0;
        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

SponzaApp::SponzaApp(HINSTANCE hInstance) : D3DApp(hInstance)
{
    mMainWndCaption = L"Sponza deferred | WASD, LMB | F flashlight | L sun | G G-buffer view | 1/2/0 tiling | 3 anim";
    mClientWidth = 1280;
    mClientHeight = 720;

    mCamera.SetPosition(0.0f, 5.0f, -20.0f);
    mCamera.LookAt(
        XMFLOAT3(0.0f, 5.0f, -20.0f),
        XMFLOAT3(0.0f, 3.0f, 0.0f),
        XMFLOAT3(0.0f, 1.0f, 0.0f));
}

SponzaApp::~SponzaApp()
{
    if (md3dDevice != nullptr)
        FlushCommandQueue();
}

bool SponzaApp::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    if (!BuildModel())
        return false;

    BuildTextures();

    // Система рендера: G-буфер, шейдеры, PSO. Под текстуры сцены — 2 SRV на материал.
    const UINT matCount = (UINT)mSponza->GetMaterials().size();
    mRenderer.Initialize(md3dDevice.Get(), mClientWidth, mClientHeight,
        mBackBufferFormat, mDepthStencilFormat, matCount * 2);
    mRendererReady = true;

    BuildMaterialSrvs();
    BuildConstantBuffers();
    BuildMaterialAnimations();
    BuildLights();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
    FlushCommandQueue();

    for (auto& tex : mTextures)
        tex->UploadHeap = nullptr;
    if (mSponzaGeo)
        mSponzaGeo->DisposeUploaders();

    return true;
}

void SponzaApp::OnResize()
{
    D3DApp::OnResize();
    mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 5000.0f);

    // G-буфер должен совпадать по размеру с окном
    if (mRendererReady)
        mRenderer.OnResize(mClientWidth, mClientHeight);
}

bool SponzaApp::KeyPressed(int vk)
{
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !mKeyWasDown[vk];
    mKeyWasDown[vk] = down;
    return pressed;
}

void SponzaApp::Update(const GameTimer& gt)
{
    const float dt = gt.DeltaTime();
    const float speed = mMoveSpeed * dt;

    if (GetAsyncKeyState('W') & 0x8000) mCamera.Walk(speed);
    if (GetAsyncKeyState('S') & 0x8000) mCamera.Walk(-speed);
    if (GetAsyncKeyState('A') & 0x8000) mCamera.Strafe(-speed);
    if (GetAsyncKeyState('D') & 0x8000) mCamera.Strafe(speed);

    if (GetAsyncKeyState('1') & 0x8000) mGlobalTiling = MathHelper::Max(0.25f, mGlobalTiling - dt);
    if (GetAsyncKeyState('2') & 0x8000) mGlobalTiling = MathHelper::Min(16.0f, mGlobalTiling + dt);
    if (GetAsyncKeyState('0') & 0x8000) mGlobalTiling = 1.0f;

    if (KeyPressed('3')) mAnimEnabled = !mAnimEnabled;
    if (KeyPressed('F')) mFlashlightOn = !mFlashlightOn;
    if (KeyPressed('L')) mSunOn = !mSunOn;
    if (KeyPressed('G')) mRenderer.SetDebugView(mRenderer.DebugView() + 1);

    if (mAnimEnabled)
        mAnimTime += dt;

    mCamera.UpdateViewMatrix();

    XMMATRIX world = XMLoadFloat4x4(&mWorld);
    XMMATRIX worldViewProj = world * mCamera.GetView() * mCamera.GetProj();

    ObjectConstants objConstants;
    XMStoreFloat4x4(&objConstants.WorldViewProj, XMMatrixTranspose(worldViewProj));
    XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
    mObjectCB->CopyData(0, objConstants);

    UpdateMaterialCB();
    UpdateLights();
    mRenderer.UpdatePassConstants(mCamera.GetPosition3f());
}

void SponzaApp::UpdateMaterialCB()
{
    const auto& materials = mSponza->GetMaterials();
    for (size_t i = 0; i < materials.size(); ++i)
    {
        const MaterialAnim& a = mMatAnims[i];

        XMMATRIX S = XMMatrixScaling(a.Tiling.x * mGlobalTiling, a.Tiling.y * mGlobalTiling, 1.0f);
        const float u = fmodf(a.ScrollSpeed.x * mAnimTime, 1.0f);
        const float v = fmodf(a.ScrollSpeed.y * mAnimTime, 1.0f);
        XMMATRIX T = XMMatrixTranslation(u, v, 0.0f);

        MaterialConstants mc;
        mc.DiffuseAlbedo = materials[i].DiffuseAlbedo;
        XMStoreFloat4x4(&mc.MatTransform, XMMatrixTranspose(S * T));
        mMaterialCB->CopyData((int)i, mc);
    }
}

void SponzaApp::UpdateLights()
{
    auto& lights = mRenderer.Lights();

    // Солнце вкл/выкл
    lights[mSunIndex].Intensity = mSunOn ? mSunIntensity : 0.0f;

    // Точечные источники плавно «покачиваются» по высоте
    for (size_t i = 0; i < mPointLightBase.size(); ++i)
    {
        XMFLOAT3 p = mPointLightBase[i];
        p.y += sinf(mAnimTime * 1.5f + (float)i) * 0.03f * mSceneHeight;
        lights[mFirstPointLight + i].Position = p;
    }

    // Фонарик — прожектор, привязанный к камере
    LightData& flash = lights[mFlashlightIndex];
    flash.Position = mCamera.GetPosition3f();
    flash.Direction = mCamera.GetLook3f();
    flash.Intensity = mFlashlightOn ? mFlashlightIntensity : 0.0f;
}

void SponzaApp::Draw(const GameTimer& gt)
{
    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    CD3DX12_RESOURCE_BARRIER toRT = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &toRT);

    SceneDrawData scene;
    scene.Geometry = mSponzaGeo.get();
    scene.Subsets = &mSponza->GetSubsets();
    scene.ObjectCB = mObjectCB->Resource()->GetGPUVirtualAddress();
    scene.MaterialCB = mMaterialCB->Resource()->GetGPUVirtualAddress();
    scene.MaterialCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));
    scene.SrvPerMaterial = 2;

    // Geometry pass + lighting pass
    mRenderer.Render(mCommandList.Get(), scene, CurrentBackBufferView(), DepthStencilView());

    CD3DX12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    FlushCommandQueue();
}

void SponzaApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;
    SetCapture(mhMainWnd);
}

void SponzaApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void SponzaApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if ((btnState & MK_LBUTTON) != 0)
    {
        float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));
        mCamera.Pitch(dy);
        mCamera.RotateY(dx);
    }
    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

//======================================================================================
// Модель и текстуры
//======================================================================================
bool SponzaApp::BuildModel()
{
    mSponza = std::make_unique<Model>();

    if (!mSponza->LoadFromOBJ("Models/sponza.obj"))
    {
        MessageBox(0, L"Failed to load Sponza model!\nCheck if model exists in Models/sponza.obj", L"Error", MB_OK);
        return false;
    }

    mSponza->CreateBuffers(md3dDevice.Get(), mCommandList.Get());
    mSponzaGeo = mSponza->GetMeshGeometry();
    return true;
}

void SponzaApp::CreateWhiteTexture()
{
    auto tex = std::make_unique<Texture>();
    tex->Name = "white";

    CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1);
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex->Resource)));

    const UINT64 uploadSize = GetRequiredIntermediateSize(tex->Resource.Get(), 0, 1);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tex->UploadHeap)));

    static const UINT32 whitePixel = 0xFFFFFFFF;
    D3D12_SUBRESOURCE_DATA data = {};
    data.pData = &whitePixel;
    data.RowPitch = 4;
    data.SlicePitch = 4;
    UpdateSubresources(mCommandList.Get(), tex->Resource.Get(), tex->UploadHeap.Get(), 0, 0, 1, &data);

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(tex->Resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1, &barrier);

    mTextures.push_back(std::move(tex));
}

int SponzaApp::LoadTexture(const std::string& path)
{
    if (path.empty())
        return 0;

    auto it = mTextureLookup.find(path);
    if (it != mTextureLookup.end())
        return it->second;

    auto tex = std::make_unique<Texture>();
    tex->Name = path;
    tex->Filename = AnsiToWString(path);

    std::string ext = path.substr(path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });

    HRESULT hr;
    if (ext == "dds")
        hr = DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(),
            tex->Filename.c_str(), tex->Resource, tex->UploadHeap);
    else
        hr = LoadTextureWIC(md3dDevice.Get(), mCommandList.Get(),
            tex->Filename.c_str(), tex->Resource, tex->UploadHeap);

    if (FAILED(hr))
    {
        OutputDebugStringA(("[Texture] NOT FOUND: " + path + "\n").c_str());
        mTextureLookup[path] = 0;
        return 0;
    }

    const int index = (int)mTextures.size();
    mTextures.push_back(std::move(tex));
    mTextureLookup[path] = index;
    return index;
}

void SponzaApp::BuildTextures()
{
    CreateWhiteTexture();

    const auto& materials = mSponza->GetMaterials();
    mMatTextures.resize(materials.size());
    for (size_t i = 0; i < materials.size(); ++i)
    {
        mMatTextures[i].Diffuse = LoadTexture(materials[i].DiffuseTexture);
        mMatTextures[i].Alpha   = LoadTexture(materials[i].AlphaTexture);
    }
}

// SRV текстур кладутся в начало кучи RenderingSystem: [diffuse, mask] на каждый материал
void SponzaApp::BuildMaterialSrvs()
{
    auto createSrv = [&](ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = res->GetDesc().Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = res->GetDesc().MipLevels;
        md3dDevice->CreateShaderResourceView(res, &srvDesc, handle);
    };

    for (UINT i = 0; i < (UINT)mMatTextures.size(); ++i)
    {
        createSrv(mTextures[mMatTextures[i].Diffuse]->Resource.Get(), mRenderer.SceneSrvCpuHandle(i * 2 + 0));
        createSrv(mTextures[mMatTextures[i].Alpha]->Resource.Get(),   mRenderer.SceneSrvCpuHandle(i * 2 + 1));
    }
}

void SponzaApp::BuildConstantBuffers()
{
    mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(md3dDevice.Get(), 1, true);
    mMaterialCB = std::make_unique<UploadBuffer<MaterialConstants>>(md3dDevice.Get(),
        (UINT)mSponza->GetMaterials().size(), true);
}

void SponzaApp::BuildMaterialAnimations()
{
    const auto& materials = mSponza->GetMaterials();
    mMatAnims.resize(materials.size());

    for (size_t i = 0; i < materials.size(); ++i)
    {
        std::string name = materials[i].Name;
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });

        if (name.find("floor") != std::string::npos)
            mMatAnims[i].Tiling = { 3.0f, 3.0f };
        else if (name.find("fabric") != std::string::npos)
            mMatAnims[i].ScrollSpeed = { 0.05f, 0.0f };
    }
}

//======================================================================================
// Источники света. Позиции задаются в долях габаритов модели,
// поэтому расстановка работает при любом масштабе Sponza.
//======================================================================================
void SponzaApp::BuildLights()
{
    const XMFLOAT3 bmin = mSponza->GetBoundsMin();
    const XMFLOAT3 bmax = mSponza->GetBoundsMax();
    const XMFLOAT3 size = { bmax.x - bmin.x, bmax.y - bmin.y, bmax.z - bmin.z };
    const bool longAlongX = size.x >= size.z;
    const float longSize = longAlongX ? size.x : size.z;
    mSceneHeight = size.y;

    // a — доля вдоль длинной стороны, h — доля высоты, b — доля поперёк
    auto P = [&](float a, float h, float b) -> XMFLOAT3
    {
        if (longAlongX)
            return { bmin.x + a * size.x, bmin.y + h * size.y, bmin.z + b * size.z };
        return { bmin.x + b * size.x, bmin.y + h * size.y, bmin.z + a * size.z };
    };

    auto normalized = [](XMFLOAT3 v) -> XMFLOAT3
    {
        XMFLOAT3 r;
        XMStoreFloat3(&r, XMVector3Normalize(XMLoadFloat3(&v)));
        return r;
    };

    auto& lights = mRenderer.Lights();
    lights.clear();
    mPointLightBase.clear();

    // ---------- 1. Directional: «солнце» сверху-сбоку ----------
    {
        LightData sun;
        sun.Type = LightDirectional;
        sun.Direction = normalized({ 0.3f, -1.0f, 0.25f });
        sun.Color = { 1.0f, 0.92f, 0.78f };
        sun.Intensity = mSunIntensity;
        mSunIndex = lights.size();
        lights.push_back(sun);
    }

    // ---------- 2. Point: 8 цветных источников вдоль галерей ----------
    {
        const XMFLOAT3 colors[8] =
        {
            { 1.0f, 0.25f, 0.15f }, { 0.2f, 1.0f, 0.3f }, { 0.3f, 0.45f, 1.0f }, { 1.0f, 0.75f, 0.2f },
            { 1.0f, 0.3f, 0.9f },   { 0.2f, 1.0f, 1.0f }, { 1.0f, 0.55f, 0.2f }, { 0.65f, 0.3f, 1.0f }
        };

        mFirstPointLight = lights.size();
        for (int i = 0; i < 8; ++i)
        {
            LightData pl;
            pl.Type = LightPoint;
            const float a = 0.12f + 0.76f * (float)(i / 2) / 3.0f;   // 4 позиции вдоль
            const float b = (i % 2 == 0) ? 0.3f : 0.7f;              // две линии поперёк
            pl.Position = P(a, 0.08f, b);
            pl.Range = 0.2f * longSize;
            pl.Color = colors[i];
            pl.Intensity = 1.6f;
            lights.push_back(pl);
            mPointLightBase.push_back(pl.Position);
        }
    }

    // ---------- 3. Spot: 3 прожектора сверху, светят вниз на пол ----------
    for (int i = 0; i < 3; ++i)
    {
        LightData sl;
        sl.Type = LightSpot;
        sl.Position = P(0.25f + 0.25f * i, 0.6f, 0.5f);
        sl.Direction = { 0.0f, -1.0f, 0.0f };
        sl.Range = 0.9f * size.y;
        sl.SpotInnerCos = cosf(XMConvertToRadians(14.0f));
        sl.SpotOuterCos = cosf(XMConvertToRadians(24.0f));
        sl.Color = { 1.0f, 0.95f, 0.85f };
        sl.Intensity = 2.5f;
        lights.push_back(sl);
    }

    // ---------- 4. Spot: фонарик камеры (позиция/направление — в UpdateLights) ----------
    {
        LightData fl;
        fl.Type = LightSpot;
        fl.Range = 0.5f * longSize;
        fl.SpotInnerCos = cosf(XMConvertToRadians(10.0f));
        fl.SpotOuterCos = cosf(XMConvertToRadians(18.0f));
        fl.Color = { 1.0f, 1.0f, 0.95f };
        fl.Intensity = mFlashlightIntensity;
        mFlashlightIndex = lights.size();
        lights.push_back(fl);
    }

    mRenderer.SetAmbient({ 0.06f, 0.06f, 0.07f, 1.0f });
    mRenderer.SetSkyColor({ 0.69f, 0.77f, 0.87f, 1.0f });
    mRenderer.SetPositionScale(8.0f / longSize);

    // Скорость камеры под масштаб сцены (пересечь Sponza примерно за 10 секунд)
    mMoveSpeed = longSize / 10.0f;
}
