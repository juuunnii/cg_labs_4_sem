#include "RenderingSystem.h"
#include "ParticleSystem.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device,
                                                       const CD3DX12_ROOT_SIGNATURE_DESC& desc)
{
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        serialized.GetAddressOf(), errors.GetAddressOf());
    if (errors)
        OutputDebugStringA((char*)errors->GetBufferPointer());
    ThrowIfFailed(hr);

    ComPtr<ID3D12RootSignature> rootSig;
    ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
    return rootSig;
}

void RenderingSystem::Initialize(ID3D12Device* device, UINT width, UINT height,
                                 DXGI_FORMAT backBufferFormat, DXGI_FORMAT depthFormat,
                                 UINT numSceneSrvs)
{
    mDevice = device;
    mBackBufferFormat = backBufferFormat;
    mDepthFormat = depthFormat;
    mNumSceneSrvs = numSceneSrvs;
    mSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Куча: [текстуры сцены ...][Albedo][Normal][Position][ShadowMap][SceneColor]
    // G-буфер, карта теней и цвет сцены лежат подряд — удобно брать одной таблицей
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = numSceneSrvs + GBuffer::Count + 2;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mSrvHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE gbufferSrv(mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        (INT)numSceneSrvs, mSrvDescriptorSize);
    mGBuffer.Initialize(device, width, height, gbufferSrv, mSrvDescriptorSize);

    CD3DX12_CPU_DESCRIPTOR_HANDLE shadowSrv(mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        (INT)(numSceneSrvs + GBuffer::Count), mSrvDescriptorSize);
    mShadowMap.Initialize(device, kShadowMapSize, shadowSrv);
    mPass.ShadowMapSize = (float)kShadowMapSize;

    mPassCB = std::make_unique<UploadBuffer<LightingPassConstants>>(device, 1, true);
    mPostCB = std::make_unique<UploadBuffer<PostConstants>>(device, 1, true);

    // RTV для текстуры «цвет сцены»
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = 1;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&mSceneColorRtvHeap)));
    BuildSceneColor(width, height);

    BuildRootSignatures();
    BuildShadersAndPSOs();
}

// Промежуточная текстура: сюда рисует lighting pass, отсюда читает пост-обработка
void RenderingSystem::BuildSceneColor(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return;

    mWidth = width;
    mHeight = height;
    mPost.InvWidth = 1.0f / (float)width;
    mPost.InvHeight = 1.0f / (float)height;

    mSceneColor.Reset();

    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(mBackBufferFormat, width, height, 1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    CD3DX12_CLEAR_VALUE clear(mBackBufferFormat, black);
    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(mDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&mSceneColor)));

    mDevice->CreateRenderTargetView(mSceneColor.Get(), nullptr,
        mSceneColorRtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = mBackBufferFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    CD3DX12_CPU_DESCRIPTOR_HANDLE h(mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        (INT)(mNumSceneSrvs + GBuffer::Count + 1), mSrvDescriptorSize);
    mDevice->CreateShaderResourceView(mSceneColor.Get(), &srv, h);
}

void RenderingSystem::OnResize(UINT width, UINT height)
{
    mGBuffer.OnResize(width, height);
    if (width != mWidth || height != mHeight)
        BuildSceneColor(width, height);
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderingSystem::SceneSrvCpuHandle(UINT index) const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        (INT)index, mSrvDescriptorSize);
}

void RenderingSystem::UpdateShadows(const XMFLOAT4X4& cameraView, const XMFLOAT3& cameraForward,
                                    float fovY, float aspect, float nearZ, float shadowDistance, float lambda,
                                    const XMFLOAT3& lightDir, const BoundingBox& sceneBounds)
{
    mShadowMap.Update(cameraView, fovY, aspect, nearZ, shadowDistance, lambda, lightDir, sceneBounds);

    for (UINT i = 0; i < CascadedShadowMap::kCascadeCount; ++i)
    {
        XMMATRIX vp = XMLoadFloat4x4(&mShadowMap.ViewProj(i));
        XMStoreFloat4x4(&mPass.ShadowViewProj[i], XMMatrixTranspose(vp));
    }
    mPass.CascadeSplits = XMFLOAT4(mShadowMap.SplitFar(0), mShadowMap.SplitFar(1),
                                   mShadowMap.SplitFar(2), mShadowMap.SplitFar(3));
    mPass.CascadeTexelWorld = XMFLOAT4(mShadowMap.TexelWorldSize(0), mShadowMap.TexelWorldSize(1),
                                       mShadowMap.TexelWorldSize(2), mShadowMap.TexelWorldSize(3));
    mPass.CameraForward = cameraForward;
}

void RenderingSystem::UpdatePassConstants(const XMFLOAT3& eyePosW)
{
    mPass.EyePosW = eyePosW;
    mPass.NumLights = (UINT)(std::min)(mLights.size(), (size_t)kMaxDeferredLights);
    for (UINT i = 0; i < mPass.NumLights; ++i)
        mPass.Lights[i] = mLights[i];

    mPassCB->CopyData(0, mPass);

    mPost.EyePosW = eyePosW;
    mPostCB->CopyData(0, mPost);
}

void RenderingSystem::BuildRootSignatures()
{
    // ---------- Geometry pass (и тени Sponza): t0..t3 текстуры, b0 кадр, b1 материал ----------
    {
        CD3DX12_DESCRIPTOR_RANGE texTable;
        texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_ALL);
        params[1].InitAsConstantBufferView(0);
        params[2].InitAsConstantBufferView(1);

        CD3DX12_STATIC_SAMPLER_DESC samplers[2] =
        {
            CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_ANISOTROPIC,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0.0f, 8),
            CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP)
        };

        CD3DX12_ROOT_SIGNATURE_DESC desc(3, params, 2, samplers,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        mGeometryRootSig = CreateRootSignature(mDevice, desc);
    }

    // ---------- Инстансинг: b0 кадр, t0 буфер экземпляров, b1 смещение ----------
    {
        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsShaderResourceView(0);
        params[2].InitAsConstants(1, 1);

        CD3DX12_ROOT_SIGNATURE_DESC desc(3, params, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        mInstancedRootSig = CreateRootSignature(mDevice, desc);
    }

    // ---------- Lighting pass: t0..t2 G-буфер, t3 карта теней, b0 свет; s0 — сэмплер сравнения ----------
    {
        CD3DX12_DESCRIPTOR_RANGE table;
        table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer::Count + 1, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsDescriptorTable(1, &table, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstantBufferView(0);

        // Сравнивающий сэмплер: возвращает долю «освещённости» с билинейной фильтрацией (аппаратный PCF 2x2)
        CD3DX12_STATIC_SAMPLER_DESC shadowSampler(
            0, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0.0f, 16,
            D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

        CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 1, &shadowSampler,
            D3D12_ROOT_SIGNATURE_FLAG_NONE);
        mLightingRootSig = CreateRootSignature(mDevice, desc);
    }

    // ---------- Пост-обработка: t0..t2 G-буфер, (t3 тени — не нужны), t4 цвет сцены; b0 параметры ----------
    {
        CD3DX12_DESCRIPTOR_RANGE table;
        table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer::Count + 2, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsDescriptorTable(1, &table, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstantBufferView(0);

        CD3DX12_STATIC_SAMPLER_DESC linearClamp(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 1, &linearClamp,
            D3D12_ROOT_SIGNATURE_FLAG_NONE);
        mPostRootSig = CreateRootSignature(mDevice, desc);
    }
}

void RenderingSystem::BuildShadersAndPSOs()
{
    mGeometryVS = d3dUtil::CompileShader(L"Shaders/GBuffer.hlsl", nullptr, "VS", "vs_5_0");
    mGeometryHS = d3dUtil::CompileShader(L"Shaders/GBuffer.hlsl", nullptr, "HS", "hs_5_0");
    mGeometryDS = d3dUtil::CompileShader(L"Shaders/GBuffer.hlsl", nullptr, "DS", "ds_5_0");
    mGeometryPS = d3dUtil::CompileShader(L"Shaders/GBuffer.hlsl", nullptr, "PS", "ps_5_0");
    mShadowPS   = d3dUtil::CompileShader(L"Shaders/GBuffer.hlsl", nullptr, "PSShadow", "ps_5_0");
    mInstancedVS = d3dUtil::CompileShader(L"Shaders/Instanced.hlsl", nullptr, "VS", "vs_5_0");
    mInstancedPS = d3dUtil::CompileShader(L"Shaders/Instanced.hlsl", nullptr, "PS", "ps_5_0");
    mLightingVS = d3dUtil::CompileShader(L"Shaders/DeferredLighting.hlsl", nullptr, "VS", "vs_5_0");
    mLightingPS = d3dUtil::CompileShader(L"Shaders/DeferredLighting.hlsl", nullptr, "PS", "ps_5_0");

    // ---------- Geometry pass PSO (тесселяция) ----------
    static const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geo = {};
    geo.InputLayout = { inputLayout, _countof(inputLayout) };
    geo.pRootSignature = mGeometryRootSig.Get();
    geo.VS = { mGeometryVS->GetBufferPointer(), mGeometryVS->GetBufferSize() };
    geo.HS = { mGeometryHS->GetBufferPointer(), mGeometryHS->GetBufferSize() };
    geo.DS = { mGeometryDS->GetBufferPointer(), mGeometryDS->GetBufferSize() };
    geo.PS = { mGeometryPS->GetBufferPointer(), mGeometryPS->GetBufferSize() };
    geo.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    geo.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    geo.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    geo.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    geo.SampleMask = UINT_MAX;
    geo.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    geo.NumRenderTargets = GBuffer::Count;
    for (UINT i = 0; i < GBuffer::Count; ++i)
        geo.RTVFormats[i] = GBuffer::Format(i);
    geo.DSVFormat = mDepthFormat;
    geo.SampleDesc.Count = 1;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&geo, IID_PPV_ARGS(&mGeometryPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC wire = geo;
    wire.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&wire, IID_PPV_ARGS(&mGeometryWirePSO)));

    // ---------- Инстансинг ----------
    static const D3D12_INPUT_ELEMENT_DESC instLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC inst = geo;
    inst.InputLayout = { instLayout, _countof(instLayout) };
    inst.pRootSignature = mInstancedRootSig.Get();
    inst.VS = { mInstancedVS->GetBufferPointer(), mInstancedVS->GetBufferSize() };
    inst.HS = { nullptr, 0 };
    inst.DS = { nullptr, 0 };
    inst.PS = { mInstancedPS->GetBufferPointer(), mInstancedPS->GetBufferSize() };
    inst.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    inst.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&inst, IID_PPV_ARGS(&mInstancedPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC instWire = inst;
    instWire.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    instWire.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&instWire, IID_PPV_ARGS(&mInstancedWirePSO)));

    // ---------- Shadow pass: только глубина (без render target'ов) ----------
    // Смещение глубины (depth bias) убирает «теневые прыщи» (shadow acne)
    D3D12_RASTERIZER_DESC shadowRaster = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    shadowRaster.CullMode = D3D12_CULL_MODE_NONE;
    shadowRaster.DepthBias = 1000;
    shadowRaster.DepthBiasClamp = 0.0f;
    shadowRaster.SlopeScaledDepthBias = 2.0f;

    // Sponza в карту теней — тот же конвейер с тесселяцией и displacement, чтобы тени совпадали с рельефом
    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadow = geo;
    shadow.PS = { mShadowPS->GetBufferPointer(), mShadowPS->GetBufferSize() };   // только alpha-test
    shadow.RasterizerState = shadowRaster;
    shadow.NumRenderTargets = 0;
    for (UINT i = 0; i < 8; ++i)
        shadow.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    shadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&shadow, IID_PPV_ARGS(&mShadowPSO)));

    // Объекты в карту теней
    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowInst = inst;
    shadowInst.PS = { nullptr, 0 };
    shadowInst.RasterizerState = shadowRaster;
    shadowInst.NumRenderTargets = 0;
    for (UINT i = 0; i < 8; ++i)
        shadowInst.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    shadowInst.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&shadowInst, IID_PPV_ARGS(&mShadowInstancedPSO)));

    // ---------- Lighting pass ----------
    D3D12_GRAPHICS_PIPELINE_STATE_DESC light = {};
    light.InputLayout = { nullptr, 0 };
    light.pRootSignature = mLightingRootSig.Get();
    light.VS = { mLightingVS->GetBufferPointer(), mLightingVS->GetBufferSize() };
    light.PS = { mLightingPS->GetBufferPointer(), mLightingPS->GetBufferSize() };
    light.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    light.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    light.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    light.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    light.DepthStencilState.DepthEnable = FALSE;
    light.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    light.SampleMask = UINT_MAX;
    light.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    light.NumRenderTargets = 1;
    light.RTVFormats[0] = mBackBufferFormat;
    light.DSVFormat = DXGI_FORMAT_UNKNOWN;
    light.SampleDesc.Count = 1;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&light, IID_PPV_ARGS(&mLightingPSO)));

    // ---------- Пост-обработка: полноэкранный квад без вершинного буфера ----------
    mPostVS = d3dUtil::CompileShader(L"Shaders/PostProcess.hlsl", nullptr, "VS", "vs_5_0");
    mPostPS = d3dUtil::CompileShader(L"Shaders/PostProcess.hlsl", nullptr, "PS", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC post = light;   // те же настройки: без глубины, 1 RT
    post.pRootSignature = mPostRootSig.Get();
    post.VS = { mPostVS->GetBufferPointer(), mPostVS->GetBufferSize() };
    post.PS = { mPostPS->GetBufferPointer(), mPostPS->GetBufferSize() };
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&post, IID_PPV_ARGS(&mPostPSO)));
}

void RenderingSystem::RenderShadowPass(ID3D12GraphicsCommandList* cmdList, const SceneDrawData& scene)
{
    mShadowMap.BeginShadowPass(cmdList);

    D3D12_VIEWPORT vp = mShadowMap.Viewport();
    D3D12_RECT sr = mShadowMap.ScissorRect();
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sr);

    for (UINT c = 0; c < CascadedShadowMap::kCascadeCount; ++c)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = mShadowMap.Dsv(c);
        cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);

        // --- Sponza ---
        if (scene.Geometry && scene.Subsets)
        {
            cmdList->SetPipelineState(mShadowPSO.Get());
            cmdList->SetGraphicsRootSignature(mGeometryRootSig.Get());
            cmdList->SetGraphicsRootConstantBufferView(1, scene.ShadowObjectCB[c]);

            D3D12_VERTEX_BUFFER_VIEW vbv = scene.Geometry->VertexBufferView();
            D3D12_INDEX_BUFFER_VIEW ibv = scene.Geometry->IndexBufferView();
            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

            for (const ModelSubset& subset : *scene.Subsets)
            {
                CD3DX12_GPU_DESCRIPTOR_HANDLE tex(mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
                    (INT)(subset.MaterialIndex * scene.SrvPerMaterial), mSrvDescriptorSize);
                cmdList->SetGraphicsRootDescriptorTable(0, tex);
                cmdList->SetGraphicsRootConstantBufferView(2,
                    scene.MaterialCB + (UINT64)subset.MaterialIndex * scene.MaterialCBByteSize);
                cmdList->DrawIndexedInstanced(subset.IndexCount, 1, subset.IndexStart, 0, 0);
            }
        }

        // --- Объекты (все, а не только видимые камерой: тень может падать в кадр из-за его края) ---
        if (scene.InstancedGeometry && scene.ShadowBatches && !scene.ShadowBatches->empty())
        {
            cmdList->SetPipelineState(mShadowInstancedPSO.Get());
            cmdList->SetGraphicsRootSignature(mInstancedRootSig.Get());
            cmdList->SetGraphicsRootConstantBufferView(0, scene.ShadowObjectCB[c]);
            cmdList->SetGraphicsRootShaderResourceView(1, scene.InstanceBuffer);

            D3D12_VERTEX_BUFFER_VIEW vbv = scene.InstancedGeometry->VertexBufferView();
            D3D12_INDEX_BUFFER_VIEW ibv = scene.InstancedGeometry->IndexBufferView();
            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            for (const InstancedBatch& b : *scene.ShadowBatches)
            {
                if (b.InstanceCount == 0) continue;
                cmdList->SetGraphicsRoot32BitConstant(2, b.InstanceOffset, 0);
                cmdList->DrawIndexedInstanced(b.IndexCount, b.InstanceCount, b.StartIndex, b.BaseVertex, 0);
            }
        }
    }

    mShadowMap.EndShadowPass(cmdList);

    // Возвращаем экранный viewport
    cmdList->RSSetViewports(1, &scene.ScreenViewport);
    cmdList->RSSetScissorRects(1, &scene.ScissorRect);
}

void RenderingSystem::Render(ID3D12GraphicsCommandList* cmdList, const SceneDrawData& scene,
                             D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv)
{
    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    //==================================================================
    // 0. SHADOW PASS — глубина из точки зрения солнца в 4 каскада
    //==================================================================
    if (mPass.ShadowsEnabled != 0)
        RenderShadowPass(cmdList, scene);

    //==================================================================
    // 1. GEOMETRY PASS — сцена в G-буфер
    //==================================================================
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    mGBuffer.BeginGeometryPass(cmdList, dsv);

    cmdList->SetPipelineState(mWireframe ? mGeometryWirePSO.Get() : mGeometryPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(1, scene.ObjectCB);

    if (scene.Geometry && scene.Subsets)
    {
        D3D12_VERTEX_BUFFER_VIEW vbv = scene.Geometry->VertexBufferView();
        D3D12_INDEX_BUFFER_VIEW ibv = scene.Geometry->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

        for (const ModelSubset& subset : *scene.Subsets)
        {
            CD3DX12_GPU_DESCRIPTOR_HANDLE tex(mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
                (INT)(subset.MaterialIndex * scene.SrvPerMaterial), mSrvDescriptorSize);
            cmdList->SetGraphicsRootDescriptorTable(0, tex);
            cmdList->SetGraphicsRootConstantBufferView(2,
                scene.MaterialCB + (UINT64)subset.MaterialIndex * scene.MaterialCBByteSize);
            cmdList->DrawIndexedInstanced(subset.IndexCount, 1, subset.IndexStart, 0, 0);
        }
    }

    if (scene.InstancedGeometry && scene.Batches && !scene.Batches->empty())
    {
        cmdList->SetGraphicsRootSignature(mInstancedRootSig.Get());
        cmdList->SetGraphicsRootConstantBufferView(0, scene.ObjectCB);
        cmdList->SetGraphicsRootShaderResourceView(1, scene.InstanceBuffer);

        D3D12_VERTEX_BUFFER_VIEW vbv = scene.InstancedGeometry->VertexBufferView();
        D3D12_INDEX_BUFFER_VIEW ibv = scene.InstancedGeometry->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (const InstancedBatch& b : *scene.Batches)
        {
            if (b.InstanceCount == 0) continue;
            const bool wire = b.Wireframe || mWireframe;
            cmdList->SetPipelineState(wire ? mInstancedWirePSO.Get() : mInstancedPSO.Get());
            cmdList->SetGraphicsRoot32BitConstant(2, b.InstanceOffset, 0);
            cmdList->DrawIndexedInstanced(b.IndexCount, b.InstanceCount, b.StartIndex, b.BaseVertex, 0);
        }
    }

    // Частицы — непрозрачные, поэтому идут в тот же G-буфер с тестом глубины (сортировка не нужна)
    if (scene.Particles)
        scene.Particles->Draw(cmdList);

    mGBuffer.EndGeometryPass(cmdList);

    //==================================================================
    // 2. LIGHTING PASS — свет + каскадные тени -> текстура «цвет сцены»
    //==================================================================
    CD3DX12_RESOURCE_BARRIER toRT = CD3DX12_RESOURCE_BARRIER::Transition(mSceneColor.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->ResourceBarrier(1, &toRT);

    D3D12_CPU_DESCRIPTOR_HANDLE sceneColorRtv = mSceneColorRtvHeap->GetCPUDescriptorHandleForHeapStart();
    cmdList->OMSetRenderTargets(1, &sceneColorRtv, TRUE, nullptr);

    cmdList->SetPipelineState(mLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(mLightingRootSig.Get());

    CD3DX12_GPU_DESCRIPTOR_HANDLE gbufferSrv(mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        (INT)mNumSceneSrvs, mSrvDescriptorSize);
    cmdList->SetGraphicsRootDescriptorTable(0, gbufferSrv);   // t0..t2 G-буфер, t3 карта теней
    cmdList->SetGraphicsRootConstantBufferView(1, mPassCB->Resource()->GetGPUVirtualAddress());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);

    CD3DX12_RESOURCE_BARRIER toSRV = CD3DX12_RESOURCE_BARRIER::Transition(mSceneColor.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &toSRV);

    //==================================================================
    // 3. POST-PROCESS — полноэкранный квад: цвет сцены + G-буфер -> back buffer
    //==================================================================
    cmdList->OMSetRenderTargets(1, &backBufferRtv, TRUE, nullptr);

    cmdList->SetPipelineState(mPostPSO.Get());
    cmdList->SetGraphicsRootSignature(mPostRootSig.Get());
    cmdList->SetGraphicsRootDescriptorTable(0, gbufferSrv);   // t0..t2 G-буфер, t4 цвет сцены
    cmdList->SetGraphicsRootConstantBufferView(1, mPostCB->Resource()->GetGPUVirtualAddress());

    // 4 вершины triangle strip = прямоугольник на весь экран; координаты строит VS по SV_VertexID
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cmdList->DrawInstanced(4, 1, 0, 0);
}
