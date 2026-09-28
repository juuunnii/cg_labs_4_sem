#include "GBuffer.h"

using Microsoft::WRL::ComPtr;

const DXGI_FORMAT GBuffer::kFormats[GBuffer::Count] =
{
    DXGI_FORMAT_R8G8B8A8_UNORM,        // Albedo
    DXGI_FORMAT_R16G16B16A16_FLOAT,    // Normal
    DXGI_FORMAT_R32G32B32A32_FLOAT     // Position
};

// Всё очищается нулями: Position.w = 0 означает «здесь нет геометрии» (небо)
const float GBuffer::kClearColors[GBuffer::Count][4] =
{
    { 0.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f }
};

void GBuffer::Initialize(ID3D12Device* device, UINT width, UINT height,
                         D3D12_CPU_DESCRIPTOR_HANDLE srvCpuStart, UINT srvDescriptorSize)
{
    mDevice = device;
    mWidth = width;
    mHeight = height;
    mSrvCpuStart = srvCpuStart;
    mSrvDescriptorSize = srvDescriptorSize;
    mRtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Отдельная куча RTV под три цели G-буфера
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = Count;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));

    BuildResources();
}

void GBuffer::OnResize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return;
    if (width == mWidth && height == mHeight)
        return;

    mWidth = width;
    mHeight = height;
    BuildResources();
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::Rtv(UINT target) const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
                                         (INT)target, mRtvDescriptorSize);
}

void GBuffer::BuildResources()
{
    for (UINT i = 0; i < Count; ++i)
    {
        mTargets[i].Reset();

        CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
            kFormats[i], mWidth, mHeight, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        CD3DX12_CLEAR_VALUE clearValue(kFormats[i], kClearColors[i]);
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

        // Стартовое состояние — «для чтения шейдером»; каждый кадр начинается с перехода в RENDER_TARGET
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
            IID_PPV_ARGS(&mTargets[i])));

        // RTV — для записи в geometry pass
        mDevice->CreateRenderTargetView(mTargets[i].Get(), nullptr, Rtv(i));

        // SRV — для чтения в lighting pass
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = kFormats[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(mSrvCpuStart, (INT)i, mSrvDescriptorSize);
        mDevice->CreateShaderResourceView(mTargets[i].Get(), &srvDesc, srvHandle);
    }
}

void GBuffer::BeginGeometryPass(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE dsv)
{
    CD3DX12_RESOURCE_BARRIER barriers[Count];
    for (UINT i = 0; i < Count; ++i)
    {
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(mTargets[i].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    cmdList->ResourceBarrier(Count, barriers);

    for (UINT i = 0; i < Count; ++i)
        cmdList->ClearRenderTargetView(Rtv(i), kClearColors[i], 0, nullptr);

    // Три RTV лежат в куче подряд — передаём первый и флаг «непрерывный диапазон»
    D3D12_CPU_DESCRIPTOR_HANDLE firstRtv = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    cmdList->OMSetRenderTargets(Count, &firstRtv, TRUE, &dsv);
}

void GBuffer::EndGeometryPass(ID3D12GraphicsCommandList* cmdList)
{
    CD3DX12_RESOURCE_BARRIER barriers[Count];
    for (UINT i = 0; i < Count; ++i)
    {
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(mTargets[i].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    cmdList->ResourceBarrier(Count, barriers);
}
