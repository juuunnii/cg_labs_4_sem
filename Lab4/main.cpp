#include "Common/d3dApp.h"
#include "Common/MathHelper.h"
#include "Common/UploadBuffer.h"
#include "Common/Camera.h"
#include "Model.h"
#include "RenderingSystem.h"
#include "SceneObjects.h"

#include <wincodec.h>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cmath>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

//======================================================================================
// Работа с картинками: декодирование через WIC, загрузка в GPU с mip-уровнями,
// построение карты нормалей из карты высот
//======================================================================================
struct ImageData
{
    UINT Width = 0;
    UINT Height = 0;
    UINT Channels = 4;              // 4 = RGBA8, 1 = R8
    std::vector<uint8_t> Pixels;
};

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

// PNG/JPG/BMP/TGA(если есть кодек) -> RGBA8
static bool DecodeImageWIC(const std::wstring& fileName, ImageData& img)
{
    IWICImagingFactory* wic = GetWICFactory();
    if (!wic) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(fileName.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return false;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter)))
        return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;

    img.Width = w;
    img.Height = h;
    img.Channels = 4;
    img.Pixels.resize(size_t(w) * h * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, w * 4, (UINT)img.Pixels.size(), img.Pixels.data()));
}

// Загружает картинку в GPU (RGBA8 или R8) с цепочкой mip-уровней (усреднение 2x2)
static HRESULT UploadImage(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    const ImageData& img, ComPtr<ID3D12Resource>& texture, ComPtr<ID3D12Resource>& uploadHeap)
{
    const UINT c = img.Channels;
    const DXGI_FORMAT format = (c == 1) ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

    std::vector<std::vector<uint8_t>> mips(1, img.Pixels);
    std::vector<UINT> mipW{ img.Width }, mipH{ img.Height };

    while (mipW.back() > 1 || mipH.back() > 1)
    {
        const UINT sw = mipW.back(), sh = mipH.back();
        const UINT dw = (std::max)(1u, sw / 2), dh = (std::max)(1u, sh / 2);

        std::vector<uint8_t> dst(size_t(dw) * dh * c);
        const std::vector<uint8_t>& src = mips.back();

        for (UINT y = 0; y < dh; ++y)
        {
            const UINT y0 = (std::min)(2 * y, sh - 1), y1 = (std::min)(2 * y + 1, sh - 1);
            for (UINT x = 0; x < dw; ++x)
            {
                const UINT x0 = (std::min)(2 * x, sw - 1), x1 = (std::min)(2 * x + 1, sw - 1);
                for (UINT k = 0; k < c; ++k)
                {
                    UINT sum = src[(size_t(y0) * sw + x0) * c + k] + src[(size_t(y0) * sw + x1) * c + k]
                             + src[(size_t(y1) * sw + x0) * c + k] + src[(size_t(y1) * sw + x1) * c + k];
                    dst[(size_t(y) * dw + x) * c + k] = (uint8_t)((sum + 2) / 4);
                }
            }
        }

        mips.push_back(std::move(dst));
        mipW.push_back(dw);
        mipH.push_back(dh);
    }
    const UINT mipCount = (UINT)mips.size();

    CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(format, img.Width, img.Height, 1, (UINT16)mipCount);
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
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
        subresources[i].RowPitch = LONG_PTR(mipW[i]) * c;
        subresources[i].SlicePitch = subresources[i].RowPitch * mipH[i];
    }
    UpdateSubresources(cmdList, texture.Get(), uploadHeap.Get(), 0, 0, mipCount, subresources.data());

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);
    return S_OK;
}

// Серая картинка (r ≈ g ≈ b) — это карта высот; цветная (голубоватая) — уже карта нормалей
static bool IsGrayscale(const ImageData& img)
{
    const size_t count = size_t(img.Width) * img.Height;
    const size_t step = (std::max)((size_t)1, count / 4096);
    size_t colored = 0, total = 0;
    for (size_t i = 0; i < count; i += step)
    {
        const int r = img.Pixels[i * 4 + 0], g = img.Pixels[i * 4 + 1], b = img.Pixels[i * 4 + 2];
        if (std::abs(r - g) > 10 || std::abs(g - b) > 10)
            ++colored;
        ++total;
    }
    return colored * 50 <= total;   // допускаем до 2% «цветных» пикселей
}

static ImageData ExtractRedChannel(const ImageData& img)
{
    ImageData out;
    out.Width = img.Width;
    out.Height = img.Height;
    out.Channels = 1;
    out.Pixels.resize(size_t(img.Width) * img.Height);
    for (size_t i = 0; i < out.Pixels.size(); ++i)
        out.Pixels[i] = img.Pixels[i * 4];
    return out;
}

// Карта нормалей из карты высот (оператор Собеля). Нормаль в касательном пространстве:
// X — вдоль U, Y — вдоль V, Z — наружу. Кодируется в [0,1]: n * 0.5 + 0.5.
static ImageData HeightToNormalMap(const ImageData& height, float strength)
{
    const int w = (int)height.Width, h = (int)height.Height;
    auto H = [&](int x, int y) -> float
    {
        x = (x % w + w) % w;   // текстуры повторяются — берём соседей «по кругу»
        y = (y % h + h) % h;
        return height.Pixels[size_t(y) * w + x] / 255.0f;
    };

    ImageData out;
    out.Width = height.Width;
    out.Height = height.Height;
    out.Channels = 4;
    out.Pixels.resize(size_t(w) * h * 4);

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const float gx = ((H(x + 1, y - 1) + 2.0f * H(x + 1, y) + H(x + 1, y + 1))
                            - (H(x - 1, y - 1) + 2.0f * H(x - 1, y) + H(x - 1, y + 1))) / 8.0f;
            const float gy = ((H(x - 1, y + 1) + 2.0f * H(x, y + 1) + H(x + 1, y + 1))
                            - (H(x - 1, y - 1) + 2.0f * H(x, y - 1) + H(x + 1, y - 1))) / 8.0f;

            float nx = -gx * strength, ny = -gy * strength, nz = 1.0f;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len; ny /= len; nz /= len;

            uint8_t* p = &out.Pixels[(size_t(y) * w + x) * 4];
            p[0] = (uint8_t)((nx * 0.5f + 0.5f) * 255.0f + 0.5f);
            p[1] = (uint8_t)((ny * 0.5f + 0.5f) * 255.0f + 0.5f);
            p[2] = (uint8_t)((nz * 0.5f + 0.5f) * 255.0f + 0.5f);
            p[3] = 255;
        }
    }
    return out;
}

static ImageData SolidImage(uint8_t r, uint8_t g, uint8_t b, uint8_t a, UINT channels)
{
    ImageData img;
    img.Width = img.Height = 1;
    img.Channels = channels;
    if (channels == 1) img.Pixels = { r };
    else               img.Pixels = { r, g, b, a };
    return img;
}

//======================================================================================
// Константные буферы geometry pass
//======================================================================================

// b0 — общий для всего кадра (совпадает с cbPerObject в GBuffer.hlsl)
struct ObjectConstants
{
    XMFLOAT4X4 World = MathHelper::Identity4x4();
    XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();

    XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    float TessMinDist = 1.0f;

    float TessMaxDist = 100.0f;
    float TessMinFactor = 1.0f;
    float TessMaxFactor = 16.0f;
    float TessMinEdgeLength = 1.0f;

    float DisplacementScale = 1.0f;
    UINT  UseNormalMap = 1;
    UINT  UseDisplacement = 1;
    UINT  UseTessellation = 1;
};

// b1 — на каждый материал (совпадает с cbMaterial в GBuffer.hlsl)
struct SurfaceConstants
{
    XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
    XMFLOAT4X4 MatTransform = MathHelper::Identity4x4();
    float DisplacementMul = 0.0f;
    XMFLOAT3 Pad = { 0.0f, 0.0f, 0.0f };
};

struct MaterialAnim
{
    XMFLOAT2 Tiling = { 1.0f, 1.0f };
    XMFLOAT2 ScrollSpeed = { 0.0f, 0.0f };
};

// 4 текстуры на материал -> t0..t3
struct MaterialTextures
{
    int Diffuse = 0;
    int Alpha = 0;
    int Normal = 0;
    int Height = 0;
    float DisplacementMul = 0.0f;
};

//======================================================================================
// Приложение
//======================================================================================
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
    int  AddTexture(const std::string& key, const ImageData& img);
    int  LoadColorTexture(const std::string& path);                        // -1 если не найдена
    void LoadHeightTexture(const std::string& path, int& heightIdx, int& normalIdx);
    void BuildTextures();
    void BuildMaterialSrvs();
    void BuildConstantBuffers();
    void BuildMaterialAnimations();
    void BuildLights();

    void UpdateObjectCB();
    void UpdateMaterialCB();
    void UpdateLights();
    void UpdateCaption();
    bool KeyPressed(int vk);

private:
    RenderingSystem mRenderer;
    bool mRendererReady = false;

    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;
    std::unique_ptr<UploadBuffer<SurfaceConstants>> mMaterialCB = nullptr;

    std::unique_ptr<Model> mSponza = nullptr;
    std::unique_ptr<MeshGeometry> mSponzaGeo = nullptr;

    // Текстуры
    std::vector<std::unique_ptr<Texture>> mTextures;
    std::unordered_map<std::string, int> mTextureLookup;
    std::unordered_map<std::string, std::pair<int, int>> mHeightLookup;   // путь -> (height, normal)
    int mWhiteTex = 0;
    int mFlatNormalTex = 0;
    int mGreyHeightTex = 0;
    std::vector<MaterialTextures> mMatTextures;
    std::vector<MaterialAnim> mMatAnims;

    XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
    Camera mCamera;
    float mMoveSpeed = 30.0f;
    POINT mLastMousePos = {};

    // Масштаб сцены (длинная сторона Sponza) — от него считаются все расстояния
    float mSceneSize = 100.0f;

    // Тесселяция / displacement / normal mapping
    bool  mUseTessellation = true;
    bool  mUseDisplacement = true;
    bool  mUseNormalMap = true;
    float mTessMaxFactor = 16.0f;
    float mDisplacementScale = 1.0f;

    // Тайлинг/анимация
    float mGlobalTiling = 1.0f;
    bool  mAnimEnabled = true;
    float mAnimTime = 0.0f;

    // Свет
    std::vector<XMFLOAT3> mPointLightBase;
    size_t mFirstPointLight = 0;
    size_t mSunIndex = 0;
    size_t mFlashlightIndex = 0;
    float  mSceneHeight = 1.0f;
    float  mSunIntensity = 1.2f;
    float  mFlashlightIntensity = 3.0f;
    bool   mSunOn = true;
    bool   mFlashlightOn = true;

    std::unordered_map<int, bool> mKeyWasDown;
    float mCaptionTimer = 0.0f;

    // ---------- Лаба 4: множество объектов и frustum culling ----------
    static const UINT kObjectCount = 8000;   // если ноутбук не тянет — уменьшите (например, до 1000)

    SceneObjects mObjects;
    bool mCullingOn = true;          // C — frustum culling вкл/выкл
    bool mUseOctree = true;          // O — через октодерево / перебором
    bool mFreezeFrustum = false;     // K — «заморозить» фрустум и посмотреть на результат со стороны
    bool mShowOctree = false;        // V — показать узлы октодерева, попавшие во фрустум
    BoundingFrustum mFrozenFrustum;

    // ---------- Лаба 5: каскадные тени ----------
    // ObjectCB: [0] — камера, [1..4] — каскады (ViewProj от солнца)
    static const UINT kObjectCBCount = 1 + CascadedShadowMap::kCascadeCount;
    bool  mShadowsOn = true;         // J — тени вкл/выкл
    bool  mShowCascades = false;     // H — подкрасить каскады
    UINT  mPcfKernel = 3;            // P — 1 / 3 / 5 / 7
    float mCascadeLambda = 0.8f;     // , / . — насколько нелинейно разбиение (0 — равномерно, 1 — логарифм)
    float mShadowDistance = 100.0f;  // до какой глубины от камеры строятся тени
    BoundingBox mSceneBounds;
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
    mMainWndCaption = L"Sponza";
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

    // 4 SRV на материал: diffuse, mask, normal, height
    const UINT matCount = (UINT)mSponza->GetMaterials().size();
    mRenderer.Initialize(md3dDevice.Get(), mClientWidth, mClientHeight,
        mBackBufferFormat, mDepthStencilFormat, matCount * 4);
    mRendererReady = true;

    BuildMaterialSrvs();
    BuildConstantBuffers();
    BuildMaterialAnimations();
    BuildLights();

    // Тысячи объектов по всему объёму Sponza + октодерево по их AABB
    mObjects.Build(md3dDevice.Get(), mCommandList.Get(),
        mSponza->GetBoundsMin(), mSponza->GetBoundsMax(), kObjectCount);

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
    FlushCommandQueue();

    for (auto& tex : mTextures)
        tex->UploadHeap = nullptr;
    if (mSponzaGeo)
        mSponzaGeo->DisposeUploaders();
    mObjects.DisposeUploaders();

    UpdateCaption();
    return true;
}

void SponzaApp::OnResize()
{
    D3DApp::OnResize();
    mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 5000.0f);

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

void SponzaApp::UpdateCaption()
{
    wchar_t buf[256];
    swprintf_s(buf,
        L"Shadows[J]:%s | PCF[P]:%ux%u | lambda[,/.]:%.2f | Cascades[H]:%s | splits %.0f / %.0f / %.0f / %.0f",
        mShadowsOn ? L"on" : L"off", mPcfKernel, mPcfKernel, mCascadeLambda,
        mShowCascades ? L"on" : L"off",
        mRenderer.CascadeSplitFar(0), mRenderer.CascadeSplitFar(1),
        mRenderer.CascadeSplitFar(2), mRenderer.CascadeSplitFar(3));
    mMainWndCaption = buf;
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

    bool captionDirty = false;
    if (KeyPressed('3')) mAnimEnabled = !mAnimEnabled;
    if (KeyPressed('F')) mFlashlightOn = !mFlashlightOn;
    if (KeyPressed('L')) mSunOn = !mSunOn;
    if (KeyPressed('G')) mRenderer.SetDebugView(mRenderer.DebugView() + 1);
    if (KeyPressed('M')) mRenderer.SetWireframe(!mRenderer.Wireframe());
    if (KeyPressed('T')) { mUseTessellation = !mUseTessellation; captionDirty = true; }
    if (KeyPressed('B')) { mUseDisplacement = !mUseDisplacement; captionDirty = true; }
    if (KeyPressed('N')) { mUseNormalMap = !mUseNormalMap;       captionDirty = true; }

    // Максимальный уровень тесселяции: '-' / '='
    if (KeyPressed(VK_OEM_MINUS)) { mTessMaxFactor = MathHelper::Max(1.0f, mTessMaxFactor / 2.0f);  captionDirty = true; }
    if (KeyPressed(VK_OEM_PLUS))  { mTessMaxFactor = MathHelper::Min(64.0f, mTessMaxFactor * 2.0f); captionDirty = true; }
    (void)captionDirty;   // заголовок теперь обновляется каждый кадр

    // Сила displacement: '[' / ']'
    if (GetAsyncKeyState(VK_OEM_4) & 0x8000) mDisplacementScale *= (1.0f - dt);
    if (GetAsyncKeyState(VK_OEM_6) & 0x8000) mDisplacementScale *= (1.0f + dt);

    // Frustum culling
    if (KeyPressed('C')) mCullingOn = !mCullingOn;
    if (KeyPressed('O')) mUseOctree = !mUseOctree;
    if (KeyPressed('V')) mShowOctree = !mShowOctree;
    if (KeyPressed('K')) mFreezeFrustum = !mFreezeFrustum;

    // Тени
    if (KeyPressed('J')) mShadowsOn = !mShadowsOn;
    if (KeyPressed('H')) mShowCascades = !mShowCascades;
    if (KeyPressed('P')) mPcfKernel = (mPcfKernel >= 7) ? 1 : mPcfKernel + 2;
    if (GetAsyncKeyState(VK_OEM_COMMA)  & 0x8000) mCascadeLambda = MathHelper::Max(0.0f, mCascadeLambda - 0.5f * dt);
    if (GetAsyncKeyState(VK_OEM_PERIOD) & 0x8000) mCascadeLambda = MathHelper::Min(1.0f, mCascadeLambda + 0.5f * dt);

    mRenderer.SetShadowsEnabled(mShadowsOn);
    mRenderer.SetShowCascades(mShowCascades);
    mRenderer.SetPcfKernel(mPcfKernel);

    if (mAnimEnabled)
        mAnimTime += dt;

    mCamera.UpdateViewMatrix();

    // Фрустум камеры в мировых координатах: строим по матрице проекции (в пространстве камеры)
    // и переносим в мир обратной матрицей вида
    BoundingFrustum frustumV, frustumW;
    BoundingFrustum::CreateFromMatrix(frustumV, mCamera.GetProj());
    XMMATRIX view = mCamera.GetView();
    XMVECTOR det = XMMatrixDeterminant(view);
    frustumV.Transform(frustumW, XMMatrixInverse(&det, view));

    if (!mFreezeFrustum)
        mFrozenFrustum = frustumW;   // пока не заморожен — запоминаем текущий

    const CullingMode mode = !mCullingOn ? CullingMode::None
                           : (mUseOctree ? CullingMode::Octree : CullingMode::BruteForce);
    mObjects.Update(mFreezeFrustum ? mFrozenFrustum : frustumW, mode, mShowOctree && mCullingOn && mUseOctree);

    // Каскады теней: делим фрустум камеры по глубине и строим матрицы солнца для каждого куска
    mRenderer.UpdateShadows(mCamera.GetView4x4f(), mCamera.GetLook3f(),
        mCamera.GetFovY(), mCamera.GetAspect(), mCamera.GetNearZ(),
        mShadowDistance, mCascadeLambda,
        mRenderer.Lights()[mSunIndex].Direction, mSceneBounds);

    UpdateCaption();

    UpdateObjectCB();
    UpdateMaterialCB();
    UpdateLights();
    mRenderer.UpdatePassConstants(mCamera.GetPosition3f());
}

void SponzaApp::UpdateObjectCB()
{
    XMMATRIX world = XMLoadFloat4x4(&mWorld);
    XMMATRIX viewProj = mCamera.GetView() * mCamera.GetProj();

    ObjectConstants oc;
    XMStoreFloat4x4(&oc.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&oc.ViewProj, XMMatrixTranspose(viewProj));
    oc.EyePosW = mCamera.GetPosition3f();

    // Все расстояния — в долях размера сцены, чтобы работало при любом масштабе модели
    oc.TessMinDist       = 0.03f * mSceneSize;   // ближе — максимальная тесселяция
    oc.TessMaxDist       = 0.40f * mSceneSize;   // дальше — тесселяции нет
    oc.TessMinFactor     = 1.0f;
    oc.TessMaxFactor     = mTessMaxFactor;
    oc.TessMinEdgeLength = 0.003f * mSceneSize;

    oc.DisplacementScale = mDisplacementScale;
    oc.UseNormalMap      = mUseNormalMap ? 1u : 0u;
    oc.UseDisplacement   = mUseDisplacement ? 1u : 0u;
    oc.UseTessellation   = mUseTessellation ? 1u : 0u;

    mObjectCB->CopyData(0, oc);

    // Для каждого каскада — те же параметры (тесселяция считается от камеры,
    // поэтому рельеф в тени совпадает с рельефом на экране), но ViewProj — от солнца
    for (UINT c = 0; c < CascadedShadowMap::kCascadeCount; ++c)
    {
        ObjectConstants sc = oc;
        XMMATRIX lightVP = XMLoadFloat4x4(&mRenderer.CascadeViewProj(c));
        XMStoreFloat4x4(&sc.ViewProj, XMMatrixTranspose(lightVP));
        mObjectCB->CopyData((int)(1 + c), sc);
    }
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

        SurfaceConstants sc;
        sc.DiffuseAlbedo = materials[i].DiffuseAlbedo;
        XMStoreFloat4x4(&sc.MatTransform, XMMatrixTranspose(S * T));
        sc.DisplacementMul = mMatTextures[i].DisplacementMul;
        mMaterialCB->CopyData((int)i, sc);
    }
}

void SponzaApp::UpdateLights()
{
    auto& lights = mRenderer.Lights();

    lights[mSunIndex].Intensity = mSunOn ? mSunIntensity : 0.0f;

    for (size_t i = 0; i < mPointLightBase.size(); ++i)
    {
        XMFLOAT3 p = mPointLightBase[i];
        p.y += sinf(mAnimTime * 1.5f + (float)i) * 0.03f * mSceneHeight;
        lights[mFirstPointLight + i].Position = p;
    }

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
    scene.MaterialCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(SurfaceConstants));
    scene.SrvPerMaterial = 4;

    // Объекты, прошедшие отсечение
    scene.InstancedGeometry = mObjects.Geometry();
    scene.InstanceBuffer = mObjects.InstanceBuffer();
    scene.Batches = &mObjects.Batches();
    scene.ShadowBatches = &mObjects.ShadowBatches();

    // Тени: b0 для каждого каскада
    const UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    for (UINT c = 0; c < CascadedShadowMap::kCascadeCount; ++c)
        scene.ShadowObjectCB[c] = scene.ObjectCB + (UINT64)(1 + c) * objCBByteSize;

    scene.ScreenViewport = mScreenViewport;
    scene.ScissorRect = mScissorRect;

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

int SponzaApp::AddTexture(const std::string& key, const ImageData& img)
{
    auto tex = std::make_unique<Texture>();
    tex->Name = key;
    ThrowIfFailed(UploadImage(md3dDevice.Get(), mCommandList.Get(), img, tex->Resource, tex->UploadHeap));

    const int index = (int)mTextures.size();
    mTextures.push_back(std::move(tex));
    return index;
}

int SponzaApp::LoadColorTexture(const std::string& path)
{
    if (path.empty())
        return -1;

    auto it = mTextureLookup.find(path);
    if (it != mTextureLookup.end())
        return it->second;

    int index = -1;
    std::string ext = path.substr(path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });

    if (ext == "dds")
    {
        auto tex = std::make_unique<Texture>();
        tex->Name = path;
        tex->Filename = AnsiToWString(path);
        if (SUCCEEDED(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(),
            tex->Filename.c_str(), tex->Resource, tex->UploadHeap)))
        {
            index = (int)mTextures.size();
            mTextures.push_back(std::move(tex));
        }
    }
    else
    {
        ImageData img;
        if (DecodeImageWIC(AnsiToWString(path), img))
            index = AddTexture(path, img);
    }

    if (index < 0)
        OutputDebugStringA(("[Texture] NOT FOUND: " + path + "\n").c_str());

    mTextureLookup[path] = index;
    return index;
}

// Серая картинка -> карта высот (R8) + построенная из неё карта нормалей.
// Если в файле уже карта нормалей (цветная) — берём её как есть, displacement нет.
void SponzaApp::LoadHeightTexture(const std::string& path, int& heightIdx, int& normalIdx)
{
    heightIdx = -1;
    normalIdx = -1;
    if (path.empty())
        return;

    auto it = mHeightLookup.find(path);
    if (it != mHeightLookup.end())
    {
        heightIdx = it->second.first;
        normalIdx = it->second.second;
        return;
    }

    ImageData img;
    if (!DecodeImageWIC(AnsiToWString(path), img))
    {
        OutputDebugStringA(("[Height] NOT FOUND: " + path + "\n").c_str());
    }
    else if (IsGrayscale(img))
    {
        ImageData height = ExtractRedChannel(img);
        heightIdx = AddTexture(path + "#height", height);
        normalIdx = AddTexture(path + "#normal", HeightToNormalMap(height, 8.0f));
        OutputDebugStringA(("[Height] height + generated normal map: " + path + "\n").c_str());
    }
    else
    {
        normalIdx = AddTexture(path + "#normal", img);
        OutputDebugStringA(("[Height] file is a normal map: " + path + "\n").c_str());
    }

    mHeightLookup[path] = { heightIdx, normalIdx };
}

void SponzaApp::BuildTextures()
{
    // Текстуры по умолчанию (1x1)
    mWhiteTex      = AddTexture("#white",  SolidImage(255, 255, 255, 255, 4));
    mFlatNormalTex = AddTexture("#normal", SolidImage(128, 128, 255, 255, 4));   // (0,0,1) — нормаль не меняется
    mGreyHeightTex = AddTexture("#height", SolidImage(128, 0, 0, 0, 1));         // 0.5 — нулевой сдвиг

    const auto& materials = mSponza->GetMaterials();
    mMatTextures.resize(materials.size());

    for (size_t i = 0; i < materials.size(); ++i)
    {
        const ModelMaterial& m = materials[i];
        MaterialTextures& mt = mMatTextures[i];

        int idx = LoadColorTexture(m.DiffuseTexture);
        mt.Diffuse = idx >= 0 ? idx : mWhiteTex;

        idx = LoadColorTexture(m.AlphaTexture);
        mt.Alpha = idx >= 0 ? idx : mWhiteTex;

        idx = LoadColorTexture(m.NormalTexture);
        mt.Normal = idx >= 0 ? idx : mFlatNormalTex;

        int heightIdx = -1, normalFromHeight = -1;
        LoadHeightTexture(m.HeightTexture, heightIdx, normalFromHeight);

        mt.Height = heightIdx >= 0 ? heightIdx : mGreyHeightTex;
        if (mt.Normal == mFlatNormalTex && normalFromHeight >= 0)
            mt.Normal = normalFromHeight;

        // Выдавливаем только непрозрачные материалы с картой высот
        // (листья и цепи с маской — тонкие плоскости, их двигать не нужно)
        mt.DisplacementMul = (heightIdx >= 0 && m.AlphaTexture.empty()) ? 1.0f : 0.0f;
    }
}

// SRV в начале кучи RenderingSystem: [diffuse, mask, normal, height] на каждый материал
void SponzaApp::BuildMaterialSrvs()
{
    auto createSrv = [&](int texIndex, UINT slot)
    {
        ID3D12Resource* res = mTextures[texIndex]->Resource.Get();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = res->GetDesc().Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = res->GetDesc().MipLevels;
        md3dDevice->CreateShaderResourceView(res, &srvDesc, mRenderer.SceneSrvCpuHandle(slot));
    };

    for (UINT i = 0; i < (UINT)mMatTextures.size(); ++i)
    {
        createSrv(mMatTextures[i].Diffuse, i * 4 + 0);
        createSrv(mMatTextures[i].Alpha,   i * 4 + 1);
        createSrv(mMatTextures[i].Normal,  i * 4 + 2);
        createSrv(mMatTextures[i].Height,  i * 4 + 3);
    }
}

void SponzaApp::BuildConstantBuffers()
{
    mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(md3dDevice.Get(), kObjectCBCount, true);
    mMaterialCB = std::make_unique<UploadBuffer<SurfaceConstants>>(md3dDevice.Get(),
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
// Источники света и масштаб сцены
//======================================================================================
void SponzaApp::BuildLights()
{
    const XMFLOAT3 bmin = mSponza->GetBoundsMin();
    const XMFLOAT3 bmax = mSponza->GetBoundsMax();
    const XMFLOAT3 size = { bmax.x - bmin.x, bmax.y - bmin.y, bmax.z - bmin.z };
    const bool longAlongX = size.x >= size.z;
    const float longSize = longAlongX ? size.x : size.z;

    mSceneHeight = size.y;
    mSceneSize = longSize;
    mDisplacementScale = 0.004f * longSize;   // высота «выпуклости» кирпичей
    mMoveSpeed = longSize / 10.0f;

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

    // Directional
    {
        LightData sun;
        sun.Type = LightDirectional;
        sun.Direction = normalized({ 0.3f, -1.0f, 0.25f });
        sun.Color = { 1.0f, 0.92f, 0.78f };
        sun.Intensity = mSunIntensity;
        mSunIndex = lights.size();
        lights.push_back(sun);
    }

    // Point x8
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
            const float a = 0.12f + 0.76f * (float)(i / 2) / 3.0f;
            const float b = (i % 2 == 0) ? 0.3f : 0.7f;
            pl.Position = P(a, 0.08f, b);
            pl.Range = 0.2f * longSize;
            pl.Color = colors[i];
            pl.Intensity = 1.6f;
            lights.push_back(pl);
            mPointLightBase.push_back(pl.Position);
        }
    }

    // Spot x3
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

    // Фонарик (spot у камеры)
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

    // Тени отбрасывает солнце; каскады покрывают глубину до 80% размера сцены
    mRenderer.SetShadowLightIndex((int)mSunIndex);
    mShadowDistance = 0.8f * longSize;
    BoundingBox::CreateFromPoints(mSceneBounds, XMLoadFloat3(&bmin), XMLoadFloat3(&bmax));
}
