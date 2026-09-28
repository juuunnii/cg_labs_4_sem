#include "CascadedShadowMap.h"
#include <cmath>
#include <cfloat>

using namespace DirectX;

void CascadedShadowMap::Initialize(ID3D12Device* device, UINT size, D3D12_CPU_DESCRIPTOR_HANDLE srvCpu)
{
    mDevice = device;
    mSize = size;

    mViewport = { 0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f };
    mScissor = { 0, 0, (LONG)size, (LONG)size };

    // Одна текстура-массив: слой на каждый каскад.
    // TYPELESS — чтобы писать в неё как в глубину (D32_FLOAT) и читать как R32_FLOAT.
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R32_TYPELESS, size, size, (UINT16)kCascadeCount, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(mDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&mShadowMap)));

    // DSV на каждый слой
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = kCascadeCount;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap)));
    mDsvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    for (UINT i = 0; i < kCascadeCount; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.MipSlice = 0;
        dsv.Texture2DArray.FirstArraySlice = i;
        dsv.Texture2DArray.ArraySize = 1;
        mDevice->CreateDepthStencilView(mShadowMap.Get(), &dsv, Dsv(i));
    }

    // SRV на весь массив — для чтения в lighting pass
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MostDetailedMip = 0;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.FirstArraySlice = 0;
    srv.Texture2DArray.ArraySize = kCascadeCount;
    mDevice->CreateShaderResourceView(mShadowMap.Get(), &srv, srvCpu);

    for (UINT i = 0; i < kCascadeCount; ++i)
        mViewProj[i] = MathHelper::Identity4x4();
}

D3D12_CPU_DESCRIPTOR_HANDLE CascadedShadowMap::Dsv(UINT cascade) const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(mDsvHeap->GetCPUDescriptorHandleForHeapStart(),
        (INT)cascade, mDsvDescriptorSize);
}

void CascadedShadowMap::BeginShadowPass(ID3D12GraphicsCommandList* cmdList)
{
    CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList->ResourceBarrier(1, &b);
}

void CascadedShadowMap::EndShadowPass(ID3D12GraphicsCommandList* cmdList)
{
    CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &b);
}

void CascadedShadowMap::Update(const XMFLOAT4X4& cameraView, float fovY, float aspect,
                               float nearZ, float shadowDistance, float lambda,
                               const XMFLOAT3& lightDir, const BoundingBox& sceneBounds)
{
    //------------------------------------------------------------------
    // 1. Нелинейное разбиение глубины на каскады
    //------------------------------------------------------------------
    const float n = nearZ;
    const float f = shadowDistance;
    mSplits[0] = n;
    for (UINT i = 1; i <= kCascadeCount; ++i)
    {
        const float p = (float)i / (float)kCascadeCount;
        const float logSplit = n * std::pow(f / n, p);
        const float uniSplit = n + (f - n) * p;
        mSplits[i] = lambda * logSplit + (1.0f - lambda) * uniSplit;
    }

    //------------------------------------------------------------------
    // 2. Базовая матрица вида солнца (смотрит вдоль направления света из начала координат).
    //    Сдвиг под каждый каскад делается в проекции — так проще «привязать» к текселям.
    //------------------------------------------------------------------
    XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&lightDir));
    XMVECTOR up = std::fabs(XMVectorGetY(L)) > 0.99f ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    XMMATRIX lightView = XMMatrixLookToLH(XMVectorZero(), L, up);

    // Диапазон глубины в пространстве света — по габаритам всей сцены,
    // чтобы тени отбрасывали и объекты вне поля зрения камеры
    XMFLOAT3 sceneCorners[8];
    sceneBounds.GetCorners(sceneCorners);
    float minZ = FLT_MAX, maxZ = -FLT_MAX;
    for (const XMFLOAT3& c : sceneCorners)
    {
        const float z = XMVectorGetZ(XMVector3TransformCoord(XMLoadFloat3(&c), lightView));
        minZ = (std::min)(minZ, z);
        maxZ = (std::max)(maxZ, z);
    }
    const float zMargin = 0.01f * (maxZ - minZ) + 1.0f;
    minZ -= zMargin;
    maxZ += zMargin;

    XMMATRIX view = XMLoadFloat4x4(&cameraView);
    XMVECTOR det = XMMatrixDeterminant(view);
    XMMATRIX invView = XMMatrixInverse(&det, view);

    const float tanY = std::tan(0.5f * fovY);
    const float tanX = tanY * aspect;

    for (UINT c = 0; c < kCascadeCount; ++c)
    {
        //--------------------------------------------------------------
        // 3. Углы «кусочка» фрустума камеры [split_c, split_c+1] в мировых координатах
        //--------------------------------------------------------------
        const float dNear = mSplits[c], dFar = mSplits[c + 1];
        XMVECTOR corners[8];
        int k = 0;
        for (float d : { dNear, dFar })
        {
            const float x = d * tanX, y = d * tanY;
            corners[k++] = XMVector3TransformCoord(XMVectorSet(-x,  y, d, 1), invView);
            corners[k++] = XMVector3TransformCoord(XMVectorSet( x,  y, d, 1), invView);
            corners[k++] = XMVector3TransformCoord(XMVectorSet( x, -y, d, 1), invView);
            corners[k++] = XMVector3TransformCoord(XMVectorSet(-x, -y, d, 1), invView);
        }

        //--------------------------------------------------------------
        // 4. Описанная сфера: её размер не меняется при повороте камеры,
        //    поэтому тени не «дрожат» при вращении
        //--------------------------------------------------------------
        XMVECTOR center = XMVectorZero();
        for (XMVECTOR v : corners) center = XMVectorAdd(center, v);
        center = XMVectorScale(center, 1.0f / 8.0f);

        float radius = 0.0f;
        for (XMVECTOR v : corners)
            radius = (std::max)(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(v, center))));
        radius = std::ceil(radius * 16.0f) / 16.0f;

        //--------------------------------------------------------------
        // 5. Центр в пространстве света, привязанный к сетке текселей —
        //    тени не «мерцают» при движении камеры
        //--------------------------------------------------------------
        const float texel = 2.0f * radius / (float)mSize;
        XMFLOAT3 cLS;
        XMStoreFloat3(&cLS, XMVector3TransformCoord(center, lightView));
        cLS.x = std::floor(cLS.x / texel) * texel;
        cLS.y = std::floor(cLS.y / texel) * texel;

        XMMATRIX proj = XMMatrixOrthographicOffCenterLH(
            cLS.x - radius, cLS.x + radius,
            cLS.y - radius, cLS.y + radius,
            minZ, maxZ);

        XMStoreFloat4x4(&mViewProj[c], lightView * proj);
        mTexelWorld[c] = texel;
    }
}
