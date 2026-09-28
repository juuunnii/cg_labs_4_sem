#include "Common/d3dApp.h"
#include "Common/MathHelper.h"
#include "Common/UploadBuffer.h"
#include "Common/Camera.h"
#include "Model.h"

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
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // WIC — это COM
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

    // 1. Декодируем картинку и переводим в RGBA 8 бит
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

    // 2. Mip-уровни: каждый следующий — усреднение 2x2 пикселей предыдущего
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

    // 3. Текстура в видеопамяти
    CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, (UINT16)mipCount);
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
    if (FAILED(hr)) return hr;

    // 4. Промежуточный upload-буфер
    const UINT64 uploadSize = GetRequiredIntermediateSize(texture.Get(), 0, mipCount);
    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    hr = device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadHeap));
    if (FAILED(hr)) return hr;

    // 5. Копирование всех mip-уровней
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

// Тайлинг и анимация одного материала
struct MaterialAnim
{
    XMFLOAT2 Tiling = { 1.0f, 1.0f };       // во сколько раз повторить текстуру
    XMFLOAT2 ScrollSpeed = { 0.0f, 0.0f };  // сдвиг UV в секунду
};

// Индексы текстур материала в mTextures (0 = белая заглушка)
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
    void BuildDescriptorHeaps();
    void BuildConstantBuffers();
    void BuildMaterialAnimations();
    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildPSO();
    void UpdateMaterialCB(const GameTimer& gt);

private:
    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    ComPtr<ID3D12DescriptorHeap> mSrvHeap = nullptr;

    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;
    std::unique_ptr<UploadBuffer<MaterialConstants>> mMaterialCB = nullptr;

    std::unique_ptr<Model> mSponza = nullptr;
    std::unique_ptr<MeshGeometry> mSponzaGeo = nullptr;

    std::vector<std::unique_ptr<Texture>> mTextures;       // [0] = белая текстура
    std::unordered_map<std::string, int> mTextureLookup;   // путь -> индекс в mTextures
    std::vector<MaterialTextures> mMatTextures;            // по материалам
    std::vector<MaterialAnim> mMatAnims;                   // по материалам

    ComPtr<ID3DBlob> mvsByteCode = nullptr;
    ComPtr<ID3DBlob> mpsByteCode = nullptr;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;
    ComPtr<ID3D12PipelineState> mPSO = nullptr;

    XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
    Camera mCamera;

    float mMoveSpeed = 30.0f;
    POINT mLastMousePos = {};

    // Управление с клавиатуры
    float mGlobalTiling = 1.0f;
    bool  mAnimEnabled = true;
    bool  mToggleKeyWasDown = false;
    float mAnimTime = 0.0f;
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
    mMainWndCaption = L"Sponza. WASD - move, LMB - look, 1/2 - tiling, 0 - reset, 3 - anim on/off";
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

    BuildTextures();              // команды копирования пишутся в открытый command list
    BuildDescriptorHeaps();       // после текстур — теперь известно, сколько SRV
    BuildConstantBuffers();
    BuildMaterialAnimations();
    BuildRootSignature();
    BuildShadersAndInputLayout();
    BuildPSO();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
    FlushCommandQueue();

    // Копирование на GPU завершено — промежуточные буферы больше не нужны
    for (auto& tex : mTextures)
        tex->UploadHeap = nullptr;
    if (mSponzaGeo)
        mSponzaGeo->DisposeUploaders();

    return true;
}

void SponzaApp::OnResize()
{
    D3DApp::OnResize();
    mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
}

void SponzaApp::Update(const GameTimer& gt)
{
    const float dt = gt.DeltaTime();
    const float speed = mMoveSpeed * dt;

    if (GetAsyncKeyState('W') & 0x8000) mCamera.Walk(speed);
    if (GetAsyncKeyState('S') & 0x8000) mCamera.Walk(-speed);
    if (GetAsyncKeyState('A') & 0x8000) mCamera.Strafe(-speed);
    if (GetAsyncKeyState('D') & 0x8000) mCamera.Strafe(speed);

    // Тайлинг: 1 — реже, 2 — чаще, 0 — сброс
    if (GetAsyncKeyState('1') & 0x8000) mGlobalTiling = MathHelper::Max(0.25f, mGlobalTiling - dt);
    if (GetAsyncKeyState('2') & 0x8000) mGlobalTiling = MathHelper::Min(16.0f, mGlobalTiling + dt);
    if (GetAsyncKeyState('0') & 0x8000) mGlobalTiling = 1.0f;

    // 3 — вкл/выкл анимацию (срабатывает один раз на нажатие)
    const bool toggleDown = (GetAsyncKeyState('3') & 0x8000) != 0;
    if (toggleDown && !mToggleKeyWasDown)
        mAnimEnabled = !mAnimEnabled;
    mToggleKeyWasDown = toggleDown;

    mCamera.UpdateViewMatrix();

    XMMATRIX world = XMLoadFloat4x4(&mWorld);
    XMMATRIX worldViewProj = world * mCamera.GetView() * mCamera.GetProj();

    ObjectConstants objConstants;
    XMStoreFloat4x4(&objConstants.WorldViewProj, XMMatrixTranspose(worldViewProj));
    XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
    mObjectCB->CopyData(0, objConstants);

    UpdateMaterialCB(gt);
}

void SponzaApp::UpdateMaterialCB(const GameTimer& gt)
{
    if (mAnimEnabled)
        mAnimTime += gt.DeltaTime();

    const auto& materials = mSponza->GetMaterials();
    for (size_t i = 0; i < materials.size(); ++i)
    {
        const MaterialAnim& a = mMatAnims[i];

        // Тайлинг — масштаб UV (сэмплер WRAP повторяет текстуру)
        XMMATRIX S = XMMatrixScaling(a.Tiling.x * mGlobalTiling, a.Tiling.y * mGlobalTiling, 1.0f);

        // Анимация — сдвиг UV во времени
        const float u = fmodf(a.ScrollSpeed.x * mAnimTime, 1.0f);
        const float v = fmodf(a.ScrollSpeed.y * mAnimTime, 1.0f);
        XMMATRIX T = XMMatrixTranslation(u, v, 0.0f);

        MaterialConstants mc;
        mc.DiffuseAlbedo = materials[i].DiffuseAlbedo;
        XMStoreFloat4x4(&mc.MatTransform, XMMatrixTranspose(S * T));

        mMaterialCB->CopyData((int)i, mc);
    }
}

void SponzaApp::Draw(const GameTimer& gt)
{
    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mPSO.Get()));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    CD3DX12_RESOURCE_BARRIER toRT = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &toRT);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferView();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = DepthStencilView();

    mCommandList->ClearRenderTargetView(rtv, Colors::LightSteelBlue, 0, nullptr);
    mCommandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    mCommandList->OMSetRenderTargets(1, &rtv, true, &dsv);

    ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

    // b0 — константы объекта
    mCommandList->SetGraphicsRootConstantBufferView(1, mObjectCB->Resource()->GetGPUVirtualAddress());

    if (mSponzaGeo)
    {
        D3D12_VERTEX_BUFFER_VIEW vbv = mSponzaGeo->VertexBufferView();
        D3D12_INDEX_BUFFER_VIEW ibv = mSponzaGeo->IndexBufferView();
        mCommandList->IASetVertexBuffers(0, 1, &vbv);
        mCommandList->IASetIndexBuffer(&ibv);
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));
        const D3D12_GPU_VIRTUAL_ADDRESS matCBBase = mMaterialCB->Resource()->GetGPUVirtualAddress();

        for (const auto& subset : mSponza->GetSubsets())
        {
            // t0, t1 — пара дескрипторов материала (diffuse, mask)
            CD3DX12_GPU_DESCRIPTOR_HANDLE tex(mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
                (INT)subset.MaterialIndex * 2, mCbvSrvUavDescriptorSize);
            mCommandList->SetGraphicsRootDescriptorTable(0, tex);

            // b1 — константы материала
            mCommandList->SetGraphicsRootConstantBufferView(2,
                matCBBase + (UINT64)subset.MaterialIndex * matCBByteSize);

            mCommandList->DrawIndexedInstanced(subset.IndexCount, 1, subset.IndexStart, 0, 0);
        }
    }

    CD3DX12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(mCommandList->Close());

    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    // Ждём GPU каждый кадр — поэтому константы безопасно перезаписывать в Update
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
// Модель
//======================================================================================
bool SponzaApp::BuildModel()
{
    mSponza = std::make_unique<Model>();

    const std::string modelPath = "Models/sponza.obj";
    if (!mSponza->LoadFromOBJ(modelPath))
    {
        MessageBox(0, L"Failed to load Sponza model!\nCheck if model exists in Models/sponza.obj", L"Error", MB_OK);
        return false;
    }

    mSponza->CreateBuffers(md3dDevice.Get(), mCommandList.Get());
    mSponzaGeo = mSponza->GetMeshGeometry();

    OutputDebugString(L"Model loaded successfully!\n");
    return true;
}

//======================================================================================
// Текстуры
//======================================================================================

// Белая текстура 1x1 — для материалов без текстуры/маски или если файл не найден
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

// Загружает текстуру (.dds — через DDSTextureLoader, остальное — через WIC).
// Возвращает индекс в mTextures, 0 — если загрузить не удалось.
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

    OutputDebugStringA(("[Texture] loaded: " + path + "\n").c_str());

    const int index = (int)mTextures.size();
    mTextures.push_back(std::move(tex));
    mTextureLookup[path] = index;
    return index;
}

void SponzaApp::BuildTextures()
{
    CreateWhiteTexture();   // индекс 0

    const auto& materials = mSponza->GetMaterials();
    mMatTextures.resize(materials.size());

    for (size_t i = 0; i < materials.size(); ++i)
    {
        mMatTextures[i].Diffuse = LoadTexture(materials[i].DiffuseTexture);
        mMatTextures[i].Alpha = LoadTexture(materials[i].AlphaTexture);

        OutputDebugStringA(("[Material] " + materials[i].Name +
            "  diffuse: " + materials[i].DiffuseTexture +
            "  alpha: " + materials[i].AlphaTexture + "\n").c_str());
    }
}

// На каждый материал — 2 соседних SRV: [diffuse, mask] -> t0, t1
void SponzaApp::BuildDescriptorHeaps()
{
    const UINT matCount = (UINT)mSponza->GetMaterials().size();

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = matCount * 2;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvHeap)));

    auto createSrv = [&](ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE handle)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = res->GetDesc().Format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = res->GetDesc().MipLevels;
            srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            md3dDevice->CreateShaderResourceView(res, &srvDesc, handle);
        };

    CD3DX12_CPU_DESCRIPTOR_HANDLE h(mSrvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < matCount; ++i)
    {
        createSrv(mTextures[mMatTextures[i].Diffuse]->Resource.Get(), h);
        h.Offset(1, mCbvSrvUavDescriptorSize);
        createSrv(mTextures[mMatTextures[i].Alpha]->Resource.Get(), h);
        h.Offset(1, mCbvSrvUavDescriptorSize);
    }
}

//======================================================================================
// Константные буферы и анимация материалов
//======================================================================================
void SponzaApp::BuildConstantBuffers()
{
    mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(md3dDevice.Get(), 1, true);
    mMaterialCB = std::make_unique<UploadBuffer<MaterialConstants>>(md3dDevice.Get(),
        (UINT)mSponza->GetMaterials().size(), true);
}

// Какие материалы тайлятся и анимируются (имена материалов — в окне «Вывод», строки [Material])
void SponzaApp::BuildMaterialAnimations()
{
    const auto& materials = mSponza->GetMaterials();
    mMatAnims.resize(materials.size());

    for (size_t i = 0; i < materials.size(); ++i)
    {
        std::string name = materials[i].Name;
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });

        auto has = [&](const char* s) { return name.find(s) != std::string::npos; };

        if (has("floor"))
            mMatAnims[i].Tiling = { 3.0f, 3.0f };          // пол — мельче плитка
        else if (has("fabric"))
            mMatAnims[i].ScrollSpeed = { 0.05f, 0.0f };    // ткани — «бегущий» узор
    }
}

//======================================================================================
// Root signature / шейдеры / PSO
//======================================================================================
void SponzaApp::BuildRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);   // t0, t1

    CD3DX12_ROOT_PARAMETER slotRootParameter[3];
    slotRootParameter[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[1].InitAsConstantBufferView(0);       // b0 cbPerObject
    slotRootParameter[2].InitAsConstantBufferView(1);       // b1 cbMaterial

    CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
        0,                                  // s0
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f, 8);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter, 1, &anisotropicWrap,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature)));
}

void SponzaApp::BuildShadersAndInputLayout()
{
    const std::wstring shaderPath = L"Shaders/sponza.hlsl";

    mvsByteCode = d3dUtil::CompileShader(shaderPath, nullptr, "VS", "vs_5_0");
    mpsByteCode = d3dUtil::CompileShader(shaderPath, nullptr, "PS", "ps_5_0");

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void SponzaApp::BuildPSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS = { reinterpret_cast<BYTE*>(mvsByteCode->GetBufferPointer()), mvsByteCode->GetBufferSize() };
    psoDesc.PS = { reinterpret_cast<BYTE*>(mpsByteCode->GetBufferPointer()), mpsByteCode->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // листья и ткани — одинарные плоскости
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = mBackBufferFormat;
    psoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    psoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    psoDesc.DSVFormat = mDepthStencilFormat;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));
}