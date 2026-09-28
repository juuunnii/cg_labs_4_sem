#pragma once
#include "Common/d3dUtil.h"

// Каскадная карта теней (Cascaded Shadow Maps) для направленного источника.
//
// Фрустум камеры делится по глубине на kCascadeCount частей. Для каждой части
// строится своя ортографическая проекция из точки зрения солнца, и сцена
// рендерится в свой слой Texture2DArray. Ближние каскады покрывают маленькую
// область — там высокая детализация теней, дальние — большую.
//
// Границы каскадов считаются по «практической» (нелинейной) схеме:
//   split_i = lambda * n*(f/n)^(i/N) + (1 - lambda) * (n + (f-n)*i/N)
// lambda = 0 — равномерное деление, lambda = 1 — чисто логарифмическое.
class CascadedShadowMap
{
public:
    static const UINT kCascadeCount = 4;

    // srvCpu — место в shader-visible куче для SRV (Texture2DArray из kCascadeCount слоёв)
    void Initialize(ID3D12Device* device, UINT size, D3D12_CPU_DESCRIPTOR_HANDLE srvCpu);

    // Пересчитывает границы каскадов и матрицы света под текущую камеру
    void Update(const DirectX::XMFLOAT4X4& cameraView, float fovY, float aspect,
                float nearZ, float shadowDistance, float lambda,
                const DirectX::XMFLOAT3& lightDir, const DirectX::BoundingBox& sceneBounds);

    void BeginShadowPass(ID3D12GraphicsCommandList* cmdList);   // -> DEPTH_WRITE
    void EndShadowPass(ID3D12GraphicsCommandList* cmdList);     // -> PIXEL_SHADER_RESOURCE

    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT cascade) const;
    D3D12_VIEWPORT Viewport() const { return mViewport; }
    D3D12_RECT ScissorRect() const { return mScissor; }
    UINT Size() const { return mSize; }

    const DirectX::XMFLOAT4X4& ViewProj(UINT cascade) const { return mViewProj[cascade]; }
    float SplitFar(UINT cascade) const { return mSplits[cascade + 1]; }       // дальняя граница (глубина в камере)
    float TexelWorldSize(UINT cascade) const { return mTexelWorld[cascade]; } // размер тексела в мире

private:
    ID3D12Device* mDevice = nullptr;
    UINT mSize = 2048;

    Microsoft::WRL::ComPtr<ID3D12Resource> mShadowMap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    UINT mDsvDescriptorSize = 0;

    D3D12_VIEWPORT mViewport = {};
    D3D12_RECT mScissor = {};

    float mSplits[kCascadeCount + 1] = {};
    float mTexelWorld[kCascadeCount] = {};
    DirectX::XMFLOAT4X4 mViewProj[kCascadeCount];
};
