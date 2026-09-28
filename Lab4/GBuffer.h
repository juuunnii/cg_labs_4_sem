#pragma once
#include "Common/d3dUtil.h"

// G-буфер: набор render target'ов, в которые geometry pass пишет
// свойства поверхности, а lighting pass их читает.
//
//   RT0  Albedo    R8G8B8A8_UNORM       цвет поверхности (sRGB), a = 1
//   RT1  Normal    R16G16B16A16_FLOAT   нормаль в мировых координатах
//   RT2  Position  R32G32B32A32_FLOAT   позиция в мировых координатах, w = 1 если есть геометрия
class GBuffer
{
public:
    enum Target : UINT { Albedo = 0, Normal = 1, Position = 2, Count = 3 };

    // srvCpuStart — место в shader-visible куче, куда класть 3 SRV (подряд)
    void Initialize(ID3D12Device* device, UINT width, UINT height,
                    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuStart, UINT srvDescriptorSize);

    // Пересоздаёт текстуры под новый размер окна (GPU должен простаивать)
    void OnResize(UINT width, UINT height);

    // Переводит RT в режим записи, очищает и привязывает их вместе с буфером глубины
    void BeginGeometryPass(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE dsv);

    // Переводит RT в режим чтения шейдером (для lighting pass)
    void EndGeometryPass(ID3D12GraphicsCommandList* cmdList);

    static DXGI_FORMAT Format(UINT target) { return kFormats[target]; }
    ID3D12Resource* Resource(UINT target) const { return mTargets[target].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT target) const;

private:
    void BuildResources();

private:
    static const DXGI_FORMAT kFormats[Count];
    static const float kClearColors[Count][4];

    ID3D12Device* mDevice = nullptr;
    UINT mWidth = 0;
    UINT mHeight = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mTargets[Count];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    UINT mRtvDescriptorSize = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE mSrvCpuStart = {};
    UINT mSrvDescriptorSize = 0;
};
