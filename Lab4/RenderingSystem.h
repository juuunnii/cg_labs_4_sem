#pragma once
#include "Common/d3dUtil.h"
#include "Common/UploadBuffer.h"
#include "GBuffer.h"
#include "Model.h"
#include <vector>
#include <memory>

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
    DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };   // point / spot
    float Range = 10.0f;                                 // point / spot: где свет гаснет до нуля
    DirectX::XMFLOAT3 Direction = { 0.0f, -1.0f, 0.0f }; // directional / spot (нормализованное)
    float SpotInnerCos = 0.95f;                          // spot: внутри — полная яркость
    DirectX::XMFLOAT3 Color = { 1.0f, 1.0f, 1.0f };
    float Intensity = 1.0f;
    int   Type = LightPoint;
    float SpotOuterCos = 0.90f;                          // spot: снаружи — темно
    DirectX::XMFLOAT2 Pad = { 0.0f, 0.0f };
};

static const int kMaxDeferredLights = 32;

// Константы lighting pass (b0 в DeferredLighting.hlsl)
struct LightingPassConstants
{
    DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    UINT NumLights = 0;
    DirectX::XMFLOAT4 Ambient = { 0.05f, 0.05f, 0.06f, 1.0f };
    DirectX::XMFLOAT4 SkyColor = { 0.69f, 0.77f, 0.87f, 1.0f };
    UINT DebugView = 0;           // 0 = итог, 1 = albedo, 2 = нормали, 3 = позиции
    float PositionScale = 0.01f;  // для отладочного вида позиций
    DirectX::XMFLOAT2 Pad = { 0.0f, 0.0f };
    LightData Lights[kMaxDeferredLights];
};

// Один вызов инстансинга: меш + диапазон в буфере экземпляров
struct InstancedBatch
{
    UINT IndexCount = 0;
    UINT StartIndex = 0;
    INT  BaseVertex = 0;
    UINT InstanceOffset = 0;   // с какого элемента StructuredBuffer читать
    UINT InstanceCount = 0;
    bool Wireframe = false;    // отладочные рамки узлов октодерева
};

// Всё, что нужно для отрисовки сцены в geometry pass
struct SceneDrawData
{
    const MeshGeometry* Geometry = nullptr;
    const std::vector<ModelSubset>* Subsets = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS ObjectCB = 0;       // b0
    D3D12_GPU_VIRTUAL_ADDRESS MaterialCB = 0;     // начало буфера материалов (b1)
    UINT MaterialCBByteSize = 0;
    UINT SrvPerMaterial = 4;                      // diffuse, mask, normal, height

    // Множество объектов, рисуемых инстансингом (лаба 4)
    const MeshGeometry* InstancedGeometry = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS InstanceBuffer = 0;             // StructuredBuffer<InstanceData>
    const std::vector<InstancedBatch>* Batches = nullptr;
};

// Deferred rendering с тесселяцией:
//   1) Geometry pass: VS -> HS -> тесселятор -> DS (displacement) -> PS (normal map) -> G-буфер
//   2) Lighting pass: полноэкранный треугольник, для каждого пикселя
//      читается G-буфер и суммируется вклад всех источников света
class RenderingSystem
{
public:
    // numSceneSrvs — сколько дескрипторов нужно под текстуры сцены.
    // Они лежат в начале общей кучи, за ними — 3 SRV G-буфера.
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
    void SetDebugView(UINT v)                    { mPass.DebugView = v % 4; }
    UINT DebugView() const                       { return mPass.DebugView; }

    void SetWireframe(bool w)                    { mWireframe = w; }
    bool Wireframe() const                       { return mWireframe; }

    // Раз в кадр: переносит источники и позицию камеры в константный буфер
    void UpdatePassConstants(const DirectX::XMFLOAT3& eyePosW);

    // Back buffer к этому моменту должен быть в состоянии RENDER_TARGET
    void Render(ID3D12GraphicsCommandList* cmdList, const SceneDrawData& scene,
                D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv);

private:
    void BuildRootSignatures();
    void BuildShadersAndPSOs();

private:
    ID3D12Device* mDevice = nullptr;
    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    GBuffer mGBuffer;

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
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mLightingPSO;

    Microsoft::WRL::ComPtr<ID3DBlob> mGeometryVS, mGeometryHS, mGeometryDS, mGeometryPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mInstancedVS, mInstancedPS;
    Microsoft::WRL::ComPtr<ID3DBlob> mLightingVS, mLightingPS;

    std::unique_ptr<UploadBuffer<LightingPassConstants>> mPassCB;
    LightingPassConstants mPass;
    std::vector<LightData> mLights;
    bool mWireframe = false;
};
