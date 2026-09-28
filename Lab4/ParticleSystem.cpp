#include "ParticleSystem.h"
#include "GBuffer.h"
#include <cmath>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static const UINT kParticleStride = sizeof(Particle);   // 64 байта
static const UINT kCountSlotSize = 256;                 // адрес CBV должен быть кратен 256

static ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heapType,
                                           D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    CD3DX12_HEAP_PROPERTIES heap(heapType);
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res)));
    return res;
}

static ComPtr<ID3D12RootSignature> CreateRootSig(ID3D12Device* device, const CD3DX12_ROOT_SIGNATURE_DESC& desc)
{
    ComPtr<ID3DBlob> serialized, errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        serialized.GetAddressOf(), errors.GetAddressOf());
    if (errors)
        OutputDebugStringA((char*)errors->GetBufferPointer());
    ThrowIfFailed(hr);

    ComPtr<ID3D12RootSignature> rs;
    ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&rs)));
    return rs;
}

//======================================================================================
// Инициализация
//======================================================================================
void ParticleSystem::Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                                UINT maxParticles, DXGI_FORMAT depthFormat)
{
    mDevice = device;
    mMaxParticles = maxParticles;
    mDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    mConstantsCB = std::make_unique<UploadBuffer<ParticleConstants>>(device, 1, true);

    BuildResources(cmdList);
    BuildDescriptors();
    BuildRootSignaturesAndPSOs(depthFormat);
}

void ParticleSystem::BuildResources(ID3D12GraphicsCommandList* cmdList)
{
    // Вспомогательный upload-буфер с начальными данными
    auto makeUpload = [&](const void* data, UINT size) -> ID3D12Resource*
    {
        ComPtr<ID3D12Resource> up = CreateBuffer(mDevice, size, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        ThrowIfFailed(up->Map(0, nullptr, &mapped));
        memcpy(mapped, data, size);
        up->Unmap(0, nullptr);
        mInitUpload.push_back(up);
        return up.Get();
    };

    const UINT zero = 0;
    ID3D12Resource* zeroUpload = makeUpload(&zero, sizeof(UINT));

    for (int i = 0; i < 2; ++i)
    {
        // Буфер частиц (StructuredBuffer с доступом UAV)
        mParticleBuffers[i] = CreateBuffer(mDevice, (UINT64)mMaxParticles * kParticleStride,
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Transition(cmdList, mParticleBuffers[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        mBufferState[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        // Счётчик Append/Consume — отдельный маленький буфер, изначально 0
        mCounters[i] = CreateBuffer(mDevice, sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Transition(cmdList, mCounters[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyBufferRegion(mCounters[i].Get(), 0, zeroUpload, 0, sizeof(UINT));
        Transition(cmdList, mCounters[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Копии счётчика для шейдеров (читаются как константный буфер)
    mCountBuffer = CreateBuffer(mDevice, 2 * kCountSlotSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    Transition(cmdList, mCountBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // Аргументы непрямой отрисовки: VertexCountPerInstance подставит GPU, InstanceCount = 1
    const D3D12_DRAW_ARGUMENTS args = { 0, 1, 0, 0 };
    ID3D12Resource* argsUpload = makeUpload(&args, sizeof(args));
    mDrawArgs = CreateBuffer(mDevice, sizeof(args), D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    Transition(cmdList, mDrawArgs.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(mDrawArgs.Get(), 0, argsUpload, 0, sizeof(args));
    Transition(cmdList, mDrawArgs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    // Readback — только для статистики в заголовке окна
    mReadback = CreateBuffer(mDevice, sizeof(UINT), D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
}

void ParticleSystem::BuildDescriptors()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 4;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mUavHeap)));

    // UAV буфера i со своим счётчиком — нужен для Append/Consume
    auto createUav = [&](int bufferIndex, UINT slot)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = 0;
        uav.Buffer.NumElements = mMaxParticles;
        uav.Buffer.StructureByteStride = kParticleStride;
        uav.Buffer.CounterOffsetInBytes = 0;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        CD3DX12_CPU_DESCRIPTOR_HANDLE h(mUavHeap->GetCPUDescriptorHandleForHeapStart(), (INT)slot, mDescriptorSize);
        mDevice->CreateUnorderedAccessView(mParticleBuffers[bufferIndex].Get(),
            mCounters[bufferIndex].Get(), &uav, h);
    };

    // Пара 0: u0 = буфер 0 (Consume), u1 = буфер 1 (Append)
    createUav(0, 0);
    createUav(1, 1);
    // Пара 1: u0 = буфер 1, u1 = буфер 0
    createUav(1, 2);
    createUav(0, 3);
}

void ParticleSystem::BuildRootSignaturesAndPSOs(DXGI_FORMAT depthFormat)
{
    // ---------- Compute: u0/u1 (таблица), b0 константы, b1 число частиц ----------
    {
        CD3DX12_DESCRIPTOR_RANGE uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsDescriptorTable(1, &uavRange);
        params[1].InitAsConstantBufferView(0);
        params[2].InitAsConstantBufferView(1);

        CD3DX12_ROOT_SIGNATURE_DESC desc(3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        mComputeRootSig = CreateRootSig(mDevice, desc);
    }

    // ---------- Отрисовка: b0 константы, t0 буфер частиц ----------
    {
        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsShaderResourceView(0);

        CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        mDrawRootSig = CreateRootSig(mDevice, desc);
    }

    const std::wstring file = L"Shaders/Particles.hlsl";
    ComPtr<ID3DBlob> emitCS   = d3dUtil::CompileShader(file, nullptr, "CSEmit",   "cs_5_0");
    ComPtr<ID3DBlob> updateCS = d3dUtil::CompileShader(file, nullptr, "CSUpdate", "cs_5_0");
    ComPtr<ID3DBlob> vs       = d3dUtil::CompileShader(file, nullptr, "VSParticle", "vs_5_0");
    ComPtr<ID3DBlob> gs       = d3dUtil::CompileShader(file, nullptr, "GSParticle", "gs_5_0");
    ComPtr<ID3DBlob> ps       = d3dUtil::CompileShader(file, nullptr, "PSParticle", "ps_5_0");

    // ---------- Compute PSO ----------
    D3D12_COMPUTE_PIPELINE_STATE_DESC cs = {};
    cs.pRootSignature = mComputeRootSig.Get();
    cs.CS = { emitCS->GetBufferPointer(), emitCS->GetBufferSize() };
    ThrowIfFailed(mDevice->CreateComputePipelineState(&cs, IID_PPV_ARGS(&mEmitPSO)));

    cs.CS = { updateCS->GetBufferPointer(), updateCS->GetBufferSize() };
    ThrowIfFailed(mDevice->CreateComputePipelineState(&cs, IID_PPV_ARGS(&mUpdatePSO)));

    // ---------- Отрисовка: точки -> GS -> билборды -> G-буфер ----------
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gfx = {};
    gfx.InputLayout = { nullptr, 0 };   // вершинного буфера нет, частица читается по SV_VertexID
    gfx.pRootSignature = mDrawRootSig.Get();
    gfx.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    gfx.GS = { gs->GetBufferPointer(), gs->GetBufferSize() };
    gfx.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    gfx.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    gfx.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gfx.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    gfx.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);   // непрозрачные: пишут глубину
    gfx.SampleMask = UINT_MAX;
    gfx.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    gfx.NumRenderTargets = GBuffer::Count;
    for (UINT i = 0; i < GBuffer::Count; ++i)
        gfx.RTVFormats[i] = GBuffer::Format(i);
    gfx.DSVFormat = depthFormat;
    gfx.SampleDesc.Count = 1;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&gfx, IID_PPV_ARGS(&mDrawPSO)));

    // ---------- ExecuteIndirect: число вершин берётся из буфера на GPU ----------
    D3D12_INDIRECT_ARGUMENT_DESC arg = {};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC sig = {};
    sig.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    sig.NumArgumentDescs = 1;
    sig.pArgumentDescs = &arg;
    ThrowIfFailed(mDevice->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&mDrawSignature)));
}

//======================================================================================
// Параметры
//======================================================================================
void ParticleSystem::SetEmitters(const std::vector<XMFLOAT3>& positions, float floorY,
                                 float sceneHeight, float sceneSize)
{
    const UINT count = (UINT)(std::min)(positions.size(), (size_t)kMaxEmitters);
    for (UINT i = 0; i < count; ++i)
        mConstants.Emitters[i] = XMFLOAT4(positions[i].x, positions[i].y, positions[i].z, 1.0f);
    mConstants.EmitterCount = count;

    // Всё — в долях размеров сцены, чтобы работало при любом масштабе модели.
    // Вершина траектории h = v^2 / (2g): при v = 0.9H и g = H частицы взлетают на ~0.4 высоты сцены
    mConstants.FloorY    = floorY;
    mConstants.Gravity   = XMFLOAT3(0.0f, -sceneHeight, 0.0f);
    mConstants.BaseSpeed = 0.9f * sceneHeight;
    mConstants.BaseSize  = 0.0025f * sceneSize;
    mConstants.Spread    = 0.35f;
    mConstants.MinLife   = 3.5f;
    mConstants.MaxLife   = 6.0f;
    mConstants.MaxParticles = mMaxParticles;
}

void ParticleSystem::Update(float dt, float totalTime, const XMFLOAT4X4& viewProj,
                            const XMFLOAT3& eyePos, const XMFLOAT3& cameraRight, const XMFLOAT3& cameraUp)
{
    dt = (std::min)(dt, 0.05f);   // после паузы окна не «выстреливаем» частицами

    // Сколько частиц родить в этом кадре (дробная часть копится)
    UINT emit = 0;
    if (mEmitting)
    {
        mEmitAccum += mEmitRate * dt;
        emit = (UINT)mEmitAccum;
        mEmitAccum -= (float)emit;
        emit = (std::min)(emit, mMaxParticles);
    }

    XMMATRIX vp = XMLoadFloat4x4(&viewProj);
    XMStoreFloat4x4(&mConstants.ViewProj, XMMatrixTranspose(vp));
    mConstants.EyePosW = eyePos;
    mConstants.CameraRight = cameraRight;
    mConstants.CameraUp = cameraUp;
    mConstants.DeltaTime = dt;
    mConstants.Time = totalTime;
    mConstants.EmitCount = emit;
    mConstants.MaxParticles = mMaxParticles;
    mConstants.Seed = ++mFrame;

    mConstantsCB->CopyData(0, mConstants);
}

//======================================================================================
// Барьеры
//======================================================================================
void ParticleSystem::Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res,
                                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(res, before, after);
    cmdList->ResourceBarrier(1, &b);
}

void ParticleSystem::TransitionParticleBuffer(ID3D12GraphicsCommandList* cmdList, int index, D3D12_RESOURCE_STATES after)
{
    Transition(cmdList, mParticleBuffers[index].Get(), mBufferState[index], after);
    mBufferState[index] = after;
}

// Счётчик буфера -> константный буфер (чтобы шейдер знал, сколько частиц обрабатывать)
void ParticleSystem::CopyCounterToCountBuffer(ID3D12GraphicsCommandList* cmdList, int counterIndex, UINT64 dstOffset)
{
    Transition(cmdList, mCounters[counterIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cmdList, mCountBuffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyBufferRegion(mCountBuffer.Get(), dstOffset, mCounters[counterIndex].Get(), 0, sizeof(UINT));

    Transition(cmdList, mCountBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    Transition(cmdList, mCounters[counterIndex].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

//======================================================================================
// Симуляция (compute)
//======================================================================================
void ParticleSystem::Simulate(ID3D12GraphicsCommandList* cmdList)
{
    const int cur = mCurrent;       // A: живые частицы + новые
    const int nxt = 1 - mCurrent;   // B: сюда попадут выжившие

    TransitionParticleBuffer(cmdList, cur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionParticleBuffer(cmdList, nxt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = { mUavHeap.Get() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(mComputeRootSig.Get());
    cmdList->SetComputeRootConstantBufferView(1, mConstantsCB->Resource()->GetGPUVirtualAddress());

    const D3D12_GPU_VIRTUAL_ADDRESS countAddr = mCountBuffer->GetGPUVirtualAddress();
    auto pairHandle = [&](int pair)
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(mUavHeap->GetGPUDescriptorHandleForHeapStart(), pair * 2, mDescriptorSize);
    };

    //--------------------------------------------------------------
    // 1. Эмиссия: Append новых частиц в A.
    //    Нужна пара, где u1 = A: для A=0 это пара 1 ([1,0]), для A=1 — пара 0 ([0,1])
    //--------------------------------------------------------------
    if (mConstants.EmitCount > 0)
    {
        CopyCounterToCountBuffer(cmdList, cur, 0);   // сколько уже живых — чтобы не переполнить буфер

        cmdList->SetPipelineState(mEmitPSO.Get());
        cmdList->SetComputeRootDescriptorTable(0, pairHandle(1 - cur));
        cmdList->SetComputeRootConstantBufferView(2, countAddr);
        cmdList->Dispatch((mConstants.EmitCount + 63) / 64, 1, 1);

        CD3DX12_RESOURCE_BARRIER uav[2] =
        {
            CD3DX12_RESOURCE_BARRIER::UAV(mParticleBuffers[cur].Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(mCounters[cur].Get())
        };
        cmdList->ResourceBarrier(2, uav);
    }

    //--------------------------------------------------------------
    // 2. Обновление: Consume из A -> физика -> Append в B.
    //    Пара с u0 = A, u1 = B: для A=0 — пара 0, для A=1 — пара 1
    //--------------------------------------------------------------
    CopyCounterToCountBuffer(cmdList, cur, kCountSlotSize);   // сколько частиц в A после эмиссии

    cmdList->SetPipelineState(mUpdatePSO.Get());
    cmdList->SetComputeRootDescriptorTable(0, pairHandle(cur));
    cmdList->SetComputeRootConstantBufferView(2, countAddr + kCountSlotSize);
    cmdList->Dispatch((mMaxParticles + 255) / 256, 1, 1);   // лишние потоки сразу выходят по счётчику

    CD3DX12_RESOURCE_BARRIER uav[2] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(mParticleBuffers[nxt].Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(mCounters[nxt].Get())
    };
    cmdList->ResourceBarrier(2, uav);

    //--------------------------------------------------------------
    // 3. Счётчик B -> число вершин для ExecuteIndirect (+ копия для статистики)
    //--------------------------------------------------------------
    Transition(cmdList, mCounters[nxt].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cmdList, mDrawArgs.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyBufferRegion(mDrawArgs.Get(), 0, mCounters[nxt].Get(), 0, sizeof(UINT));
    cmdList->CopyBufferRegion(mReadback.Get(), 0, mCounters[nxt].Get(), 0, sizeof(UINT));

    Transition(cmdList, mDrawArgs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    Transition(cmdList, mCounters[nxt].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // B будет читать вершинный шейдер
    TransitionParticleBuffer(cmdList, nxt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Меняем буферы местами: в следующем кадре B станет A.
    // Счётчик старого A после Consume уже равен 0 — сбрасывать не нужно.
    mDrawBuffer = nxt;
    mCurrent = nxt;
}

//======================================================================================
// Отрисовка в G-буфер
//======================================================================================
void ParticleSystem::Draw(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->SetPipelineState(mDrawPSO.Get());
    cmdList->SetGraphicsRootSignature(mDrawRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, mConstantsCB->Resource()->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootShaderResourceView(1, mParticleBuffers[mDrawBuffer]->GetGPUVirtualAddress());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

    // Число точек лежит в mDrawArgs на GPU — CPU его не знает и знать не должен
    cmdList->ExecuteIndirect(mDrawSignature.Get(), 1, mDrawArgs.Get(), 0, nullptr, 0);
}

void ParticleSystem::ReadBackAliveCount()
{
    D3D12_RANGE readRange = { 0, sizeof(UINT) };
    void* data = nullptr;
    if (SUCCEEDED(mReadback->Map(0, &readRange, &data)))
    {
        mAliveCount = *reinterpret_cast<UINT*>(data);
        D3D12_RANGE writeRange = { 0, 0 };
        mReadback->Unmap(0, &writeRange);
    }
}
