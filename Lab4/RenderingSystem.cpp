#include "RenderingSystem.h"

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

    // Общая shader-visible куча: [текстуры сцены ...][Albedo][Normal][Position]
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = numSceneSrvs + GBuffer::Count;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mSrvHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE gbufferSrv(mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        (INT)numSceneSrvs, mSrvDescriptorSize);
    mGBuffer.Initialize(device, width, height, gbufferSrv, mSrvDescriptorSize);

    mPassCB = std::make_unique<UploadBuffer<LightingPassConstants>>(device, 1, true);

    BuildRootSignatures();
    BuildShadersAndPSOs();
}

void RenderingSystem::OnResize(UINT width, UINT height)
{
    mGBuffer.OnResize(width, height);
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderingSystem::SceneSrvCpuHandle(UINT index) const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        (INT)index, mSrvDescriptorSize);
}

void RenderingSystem::UpdatePassConstants(const XMFLOAT3& eyePosW)
{
    mPass.EyePosW = eyePosW;
    mPass.NumLights = (UINT)(std::min)(mLights.size(), (size_t)kMaxDeferredLights);
    for (UINT i = 0; i < mPass.NumLights; ++i)
        mPass.Lights[i] = mLights[i];

    mPassCB->CopyData(0, mPass);
}

void RenderingSystem::BuildRootSignatures()
{
    // ---------- Geometry pass: t0 diffuse, t1 mask, t2 normal, t3 height; b0 кадр, b1 материал ----------
    // Видимость ALL: карту высот читает domain shader, остальные — pixel shader
    {
        CD3DX12_DESCRIPTOR_RANGE texTable;
        texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_ALL);
        params[1].InitAsConstantBufferView(0);
        params[2].InitAsConstantBufferView(1);

        CD3DX12_STATIC_SAMPLER_DESC samplers[2] =
        {
            // s0 — для обычных текстур в pixel shader
            CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_ANISOTROPIC,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0.0f, 8),
                // s1 — для SampleLevel в domain shader
                CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                    D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                    D3D12_TEXTURE_ADDRESS_MODE_WRAP)
        };

        CD3DX12_ROOT_SIGNATURE_DESC desc(3, params, 2, samplers,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        mGeometryRootSig = CreateRootSignature(mDevice, desc);
    }

    // ---------- Lighting pass: t0..t2 G-буфер, b0 источники света ----------
    {
        CD3DX12_DESCRIPTOR_RANGE gbufferTable;
        gbufferTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer::Count, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsDescriptorTable(1, &gbufferTable, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstantBufferView(0);

        CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_NONE);
        mLightingRootSig = CreateRootSignature(mDevice, desc);
    }
}

void RenderingSystem::BuildShadersAndPSOs()
{
    mGeometryVS = d3dUtil::CompileShader(L"Shaders/GBuffer.hlsl", nullptr, "VS", "vs_5_0");
    mGeometryHS = d3dUtil::CompileShader(L"Shaders/GBuffer.hlsl", nullptr, "HS", "hs_5_0");
    mGeometryDS = d3dUtil::CompileShader(L"Shaders/GBuffer.hlsl", nullptr, "DS", "ds_5_0");
    mGeometryPS = d3dUtil::CompileShader(L"Shaders/GBuffer.hlsl", nullptr, "PS", "ps_5_0");
    mLightingVS = d3dUtil::CompileShader(L"Shaders/DeferredLighting.hlsl", nullptr, "VS", "vs_5_0");
    mLightingPS = d3dUtil::CompileShader(L"Shaders/DeferredLighting.hlsl", nullptr, "PS", "ps_5_0");

    // ---------- Geometry pass PSO: тесселяция, 3 render target'а + глубина ----------
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
    geo.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // листья и ткани — одинарные плоскости
    geo.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    geo.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    geo.SampleMask = UINT_MAX;
    geo.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;   // вход тесселяции — патчи
    geo.NumRenderTargets = GBuffer::Count;
    for (UINT i = 0; i < GBuffer::Count; ++i)
        geo.RTVFormats[i] = GBuffer::Format(i);
    geo.DSVFormat = mDepthFormat;
    geo.SampleDesc.Count = 1;
    geo.SampleDesc.Quality = 0;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&geo, IID_PPV_ARGS(&mGeometryPSO)));

    // Тот же PSO, но каркасом — чтобы видеть, как меняется тесселяция с расстоянием
    D3D12_GRAPHICS_PIPELINE_STATE_DESC wire = geo;
    wire.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&wire, IID_PPV_ARGS(&mGeometryWirePSO)));

    // ---------- Lighting pass PSO: полноэкранный треугольник, без вершинного буфера и глубины ----------
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
    light.SampleDesc.Quality = 0;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&light, IID_PPV_ARGS(&mLightingPSO)));
}

void RenderingSystem::Render(ID3D12GraphicsCommandList* cmdList, const SceneDrawData& scene,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv)
{
    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

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
        // Каждый треугольник — патч из 3 контрольных точек для hull shader
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

    mGBuffer.EndGeometryPass(cmdList);

    //==================================================================
    // 2. LIGHTING PASS — полноэкранный треугольник в back buffer
    //==================================================================
    cmdList->OMSetRenderTargets(1, &backBufferRtv, TRUE, nullptr);

    cmdList->SetPipelineState(mLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(mLightingRootSig.Get());

    CD3DX12_GPU_DESCRIPTOR_HANDLE gbufferSrv(mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        (INT)mNumSceneSrvs, mSrvDescriptorSize);
    cmdList->SetGraphicsRootDescriptorTable(0, gbufferSrv);
    cmdList->SetGraphicsRootConstantBufferView(1, mPassCB->Resource()->GetGPUVirtualAddress());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);   // вершины генерируются в шейдере по SV_VertexID
}