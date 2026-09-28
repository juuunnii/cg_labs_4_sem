#pragma once
#include "Common/d3dUtil.h"
#include "Common/UploadBuffer.h"
#include "GBuffer.h"
#include "CascadedShadowMap.h"
#include "Model.h"
#include <vector>
#include <memory>
#include <string>

class ParticleSystem;   // лаба 6: рисуется в geometry pass

// Типы источников (совпадают с #define в Shaders/DeferredLighting.hlsl)
enum LightType : int
{
    LightDirectional = 0,
    LightPoint = 1,
    LightSpot = 2
};

// Один источник света. Раскладка = 4 x float4 (64 байта), совпадает с HLSL.
struct LightData
{
    DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
    float Range = 10.0f;
    DirectX::XMFLOAT3 Direction = { 0.0f, -1.0f, 0.0f };
    float SpotInnerCos = 0.95f;
    DirectX::XMFLOAT3 Color = { 1.0f, 1.0f, 1.0f };
    float Intensity = 1.0f;
    int   Type = LightPoint;
    float SpotOuterCos = 0.90f;
    DirectX::XMFLOAT2 Pad = { 0.0f, 0.0f };
};

static const int kMaxDeferredLights = 32;

// Константы lighting pass (b0 в DeferredLighting.hlsl) — порядок полей важен!
struct LightingPassConstants
{
    DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    UINT NumLights = 0;
    DirectX::XMFLOAT4 Ambient = { 0.05f, 0.05f, 0.06f, 1.0f };
    DirectX::XMFLOAT4 SkyColor = { 0.69f, 0.77f, 0.87f, 1.0f };
    UINT DebugView = 0;
    float PositionScale = 0.01f;
    DirectX::XMFLOAT2 Pad = { 0.0f, 0.0f };

    // ---- Каскадные тени ----
    DirectX::XMFLOAT4X4 ShadowViewProj[CascadedShadowMap::kCascadeCount];
    DirectX::XMFLOAT4 CascadeSplits = { 0, 0, 0, 0 };       // дальняя граница каждого каскада
    DirectX::XMFLOAT4 CascadeTexelWorld = { 0, 0, 0, 0 };   // размер тексела в мире
    DirectX::XMFLOAT3 CameraForward = { 0.0f, 0.0f, 1.0f };
    UINT ShadowsEnabled = 1;
    UINT PcfKernel = 3;          // 1, 3, 5 или 7
    UINT ShowCascades = 0;       // подкрасить каскады цветом
    int  ShadowLightIndex = 0;   // какой источник отбрасывает тени
    float ShadowMapSize = 2048.0f;

    // ---- Лаба 8: PBR + IBL ----
    DirectX::XMFLOAT4X4 InvViewProj;          // экран -> мир (направление взгляда для неба)
    float InvScreenWidth = 1.0f / 1280.0f;
    float InvScreenHeight = 1.0f / 720.0f;
    float PrefilteredMaxLod = 4.0f;           // последний mip pre-filtered map (= roughness 1)
    float IBLIntensity = 0.5f;
    UINT  IBLEnabled = 1;
    UINT  MaterialOverride = 0;               // 0 — как в G-буфере, 1 — гладкий металл, 2 — матовый диэлектрик
    float LightScale = 3.14159265f;           // перевод «яркости» источников в radiance (см. шейдер)
    float Exposure = 1.0f;                    // экспозиция перед тонмаппингом (HDR -> LDR)

    LightData Lights[kMaxDeferredLights];
};

// Константы пост-обработки (b0 в Shaders/PostProcess.hlsl) — порядок полей важен!
struct PostConstants
{
    DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    float Time = 0.0f;

    DirectX::XMFLOAT4 FogColor = { 0.70f, 0.74f, 0.80f, 1.0f };

    float FogStart = 10.0f;            // с какого расстояния начинается туман
    float FogEnd = 100.0f;             // где он максимален
    float FogBaseY = 0.0f;             // уровень пола
    float FogHeightFalloff = 0.1f;     // как быстро туман редеет с высотой

    float FogDensity = 0.85f;          // максимальная плотность
    float OutlineThickness = 1.0f;     // толщина контура в пикселях
    float OutlineDepthScale = 12.0f;   // чувствительность к перепаду глубины
    float OutlineNormalScale = 1.5f;   // чувствительность к перепаду нормалей

    DirectX::XMFLOAT4 OutlineColor = { 0.05f, 0.04f, 0.03f, 1.0f };

    float VignetteStrength = 0.55f;
    float ChromaticAmount = 0.006f;
    float InvWidth = 1.0f / 1280.0f;
    float InvHeight = 1.0f / 720.0f;

    UINT FogEnabled = 1;
    UINT OutlineEnabled = 1;
    UINT VignetteEnabled = 1;
    UINT ChromaticEnabled = 1;
};

// Один вызов инстансинга
struct InstancedBatch
{
    UINT IndexCount = 0;
    UINT StartIndex = 0;
    INT  BaseVertex = 0;
    UINT InstanceOffset = 0;
    UINT InstanceCount = 0;
    bool Wireframe = false;
};

// Всё, что нужно для отрисовки кадра
struct SceneDrawData
{
    // Sponza
    const MeshGeometry* Geometry = nullptr;
    const std::vector<ModelSubset>* Subsets = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS ObjectCB = 0;       // b0 для камеры
    D3D12_GPU_VIRTUAL_ADDRESS MaterialCB = 0;
    UINT MaterialCBByteSize = 0;
    UINT SrvPerMaterial = 4;

    // Объекты (инстансинг)
    const MeshGeometry* InstancedGeometry = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS InstanceBuffer = 0;
    const std::vector<InstancedBatch>* Batches = nullptr;         // видимые камерой
    const std::vector<InstancedBatch>* ShadowBatches = nullptr;   // все (отбрасывают тени)

    // Тени: b0 для каждого каскада (тот же формат, что ObjectCB, но ViewProj — от солнца)
    D3D12_GPU_VIRTUAL_ADDRESS ShadowObjectCB[CascadedShadowMap::kCascadeCount] = {};

    // Экранный viewport — восстанавливается после shadow pass
    D3D12_VIEWPORT ScreenViewport = {};
    D3D12_RECT ScissorRect = {};

    // Частицы (непрозрачные, пишутся в G-буфер); nullptr — не рисовать
    ParticleSystem* Particles = nullptr;
};

// Deferred rendering:
//   0) Shadow pass: глубина сцены из точки зрения солнца в каждый каскад
//   1) Geometry pass: VS -> HS -> DS (displacement) -> PS (normal map) -> G-буфер
//   2) Lighting pass: полноэкранный треугольник, свет + каскадные тени с PCF
class RenderingSystem
{
public:
    void Initialize(ID3D12Device* device, UINT width, UINT height,
                    DXGI_FORMAT backBufferFormat, DXGI_FORMAT depthFormat,
                    UINT numSceneSrvs);

    void OnResize(UINT width, UINT height);

    ID3D12DescriptorHeap* SrvHeap() const { return mSrvHeap.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE SceneSrvCpuHandle(UINT index) const;

    std::vector<LightData>& Lights() { return mLights; }

    void SetAmbient(const DirectX::XMFLOAT4& c)  { mPass.Ambient = c; }
    void SetSkyColor(const DirectX::XMFLOAT4& c) { mPass.SkyColor = c; }
    void SetPositionScale(float s)               { mPass.PositionScale = s; }
    void SetDebugView(UINT v)                    { mPass.DebugView = v % 5; }   // 4 — roughness/metallic
    UINT DebugView() const                       { return mPass.DebugView; }

    void SetWireframe(bool w)                    { mWireframe = w; }
    bool Wireframe() const                       { return mWireframe; }

    // ---- Тени ----
    void SetShadowsEnabled(bool e)               { mPass.ShadowsEnabled = e ? 1u : 0u; }
    bool ShadowsEnabled() const                  { return mPass.ShadowsEnabled != 0; }
    void SetPcfKernel(UINT k)                    { mPass.PcfKernel = k; }
    UINT PcfKernel() const                       { return mPass.PcfKernel; }
    void SetShowCascades(bool s)                 { mPass.ShowCascades = s ? 1u : 0u; }
    bool ShowCascades() const                    { return mPass.ShowCascades != 0; }
    void SetShadowLightIndex(int i)              { mPass.ShadowLightIndex = i; }

    // Пересчёт каскадов под камеру; после вызова доступны CascadeViewProj()
    void UpdateShadows(const DirectX::XMFLOAT4X4& cameraView, const DirectX::XMFLOAT3& cameraForward,
                       float fovY, float aspect, float nearZ, float shadowDistance, float lambda,
                       const DirectX::XMFLOAT3& lightDir, const DirectX::BoundingBox& sceneBounds);
    const DirectX::XMFLOAT4X4& CascadeViewProj(UINT i) const { return mShadowMap.ViewProj(i); }
    float CascadeSplitFar(UINT i) const { return mShadowMap.SplitFar(i); }

    // ---- Пост-обработка ----
    PostConstants& Post() { return mPost; }

    // ---- Лаба 8: IBL ----
    // Загружает irradiance.dds, prefiltered.dds, brdf_lut.dds из папки dir.
    // Команды копирования пишутся в cmdList — его нужно выполнить и дождаться (FlushCommandQueue),
    // после чего вызвать DisposeUploaders().
    bool LoadIBL(ID3D12GraphicsCommandList* cmdList, const std::wstring& dir);
    void DisposeUploaders();
    bool IBLLoaded() const                       { return mIblLoaded; }
    void SetIBLEnabled(bool e)                   { mIblEnabled = e; }
    void SetIBLIntensity(float i)                { mPass.IBLIntensity = i; }
    void SetMaterialOverride(UINT m)             { mPass.MaterialOverride = m; }
    void SetInvViewProj(const DirectX::XMFLOAT4X4& invViewProj);

    void UpdatePassConstants(const DirectX::XMFLOAT3& eyePosW);

    void Render(ID3D12GraphicsCommandList* cmdList, const SceneDrawData& scene,
                D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv);

private:
    void BuildRootSignatures();
    void BuildShadersAndPSOs();
    void BuildSceneColor(UINT width, UINT height);
    void CreateIBLSrvs();
    void RenderShadowPass(ID3D12GraphicsCommandList* cmdList, const SceneDrawData& scene);

private:
    static const UINT kShadowMapSize = 2048;

    ID3D12Device* mDevice = nullptr;
    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    GBuffer mGBuffer;
    CascadedShadowMap mShadowMap;

    // Куча: [текстуры сцены][Albedo][Normal][Position][ShadowMap][IBL x3][SceneColor]
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mSrvHeap;
    UINT mSrvDescriptorSize = 0;
    UINT mNumSceneSrvs = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mGeometryRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mInstancedRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mLightingRootSig;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGeometryPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGeometryWirePSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mInstancedPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mInstancedWirePSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mShadowPSO;            // Sponza в карту теней
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mShadowInstancedPSO;   // объекты в карту теней
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mLightingPSO;

    Microsoft::WRL::ComPtr<ID3DBlob> mGeometryVS, mGeometryHS, mGeometryDS, mGeometryPS, mShadowPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mInstancedVS, mInstancedPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mLightingVS, mLightingPS;

    std::unique_ptr<UploadBuffer<LightingPassConstants>> mPassCB;
    LightingPassConstants mPass;
    std::vector<LightData> mLights;
    bool mWireframe = false;

    // ---- Пост-обработка ----
    // Lighting pass рисует в mSceneColor, пост-проход читает её (+ G-буфер) и пишет в back buffer.
    UINT mWidth = 0;
    UINT mHeight = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSceneColor;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mSceneColorRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mPostRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPostPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> mPostVS, mPostPS;
    std::unique_ptr<UploadBuffer<PostConstants>> mPostCB;
    PostConstants mPost;

    // ---- Лаба 8: IBL ----
    // Куча: [текстуры сцены][Albedo][Normal][Position][ShadowMap][Irradiance][Prefiltered][BRDF LUT][SceneColor]
    enum IblSlot : UINT { IblIrradiance = 0, IblPrefiltered, IblBrdfLut, IblCount };
    Microsoft::WRL::ComPtr<ID3D12Resource> mIblTex[IblCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> mIblUpload[IblCount];
    bool mIblLoaded = false;
    bool mIblEnabled = true;
    UINT IblSrvIndex(UINT slot) const { return mNumSceneSrvs + GBuffer::Count + 1 + slot; }
    UINT SceneColorSrvIndex() const   { return mNumSceneSrvs + GBuffer::Count + 1 + IblCount; }
};
