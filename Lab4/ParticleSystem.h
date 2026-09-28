#pragma once
#include "Common/d3dUtil.h"
#include "Common/UploadBuffer.h"
#include <vector>
#include <memory>

// Одна частица (совпадает со struct Particle в Shaders/Particles.hlsl, 64 байта)
struct Particle
{
    DirectX::XMFLOAT3 Position;
    float Age;
    DirectX::XMFLOAT3 Velocity;
    float Lifetime;
    DirectX::XMFLOAT4 Color;
    float Size;
    DirectX::XMFLOAT3 Pad;
};

// Константы симуляции и отрисовки (cbSim, b0 в Particles.hlsl) — порядок полей важен!
struct ParticleConstants
{
    DirectX::XMFLOAT4X4 ViewProj;

    DirectX::XMFLOAT3 EyePosW;      float DeltaTime;
    DirectX::XMFLOAT3 CameraRight;  float Time;
    DirectX::XMFLOAT3 CameraUp;     UINT  EmitCount;
    DirectX::XMFLOAT3 Gravity;      UINT  MaxParticles;

    DirectX::XMFLOAT4 Emitters[4];  // xyz — позиция фонтана

    UINT  EmitterCount;
    float FloorY;
    float BaseSize;
    float BaseSpeed;

    float Spread;
    float MinLife;
    float MaxLife;
    UINT  Seed;
};

// Система частиц полностью на GPU.
//
// Два буфера частиц A и B со счётчиками (UAV counter). Каждый кадр:
//   1) CSEmit   — новые частицы добавляются (Append) в A
//   2) CSUpdate — каждая частица забирается (Consume) из A, обновляется,
//                 и если ещё жива — добавляется (Append) в B
//   3) счётчик B копируется в аргументы ExecuteIndirect
//   4) отрисовка B: VS читает частицу, GS разворачивает точку в билборд,
//      PS пишет непрозрачную «сферу» в G-буфер
//   5) A и B меняются местами
// CPU ни разу не читает и не пишет сами частицы.
class ParticleSystem
{
public:
    static const UINT kMaxEmitters = 4;

    void Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                    UINT maxParticles, DXGI_FORMAT depthFormat);

    // Фонтаны: позиции, уровень пола, высота сцены (от неё считаются скорости и размеры)
    void SetEmitters(const std::vector<DirectX::XMFLOAT3>& positions, float floorY,
                     float sceneHeight, float sceneSize);

    void SetEmitRate(float particlesPerSecond) { mEmitRate = particlesPerSecond; }
    float EmitRate() const { return mEmitRate; }
    void SetEmitting(bool e) { mEmitting = e; }
    bool Emitting() const { return mEmitting; }
    UINT MaxParticles() const { return mMaxParticles; }

    // Раз в кадр на CPU: заполняет константы (сколько родить, камера для билбордов)
    void Update(float dt, float totalTime, const DirectX::XMFLOAT4X4& viewProj,
                const DirectX::XMFLOAT3& eyePos, const DirectX::XMFLOAT3& cameraRight,
                const DirectX::XMFLOAT3& cameraUp);

    // Compute-проходы (вызывать до geometry pass)
    void Simulate(ID3D12GraphicsCommandList* cmdList);

    // Отрисовка в G-буфер (вызывается внутри geometry pass)
    void Draw(ID3D12GraphicsCommandList* cmdList);

    // Число живых частиц (с прошлого кадра) — читается из readback-буфера после FlushCommandQueue
    void ReadBackAliveCount();
    UINT AliveCount() const { return mAliveCount; }

    void DisposeUploaders() { mInitUpload.clear(); }

private:
    void BuildResources(ID3D12GraphicsCommandList* cmdList);
    void BuildDescriptors();
    void BuildRootSignaturesAndPSOs(DXGI_FORMAT depthFormat);

    void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void TransitionParticleBuffer(ID3D12GraphicsCommandList* cmdList, int index, D3D12_RESOURCE_STATES after);
    void CopyCounterToCountBuffer(ID3D12GraphicsCommandList* cmdList, int counterIndex, UINT64 dstOffset);

private:
    ID3D12Device* mDevice = nullptr;
    UINT mMaxParticles = 0;

    // Два буфера частиц и их счётчики
    Microsoft::WRL::ComPtr<ID3D12Resource> mParticleBuffers[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> mCounters[2];
    D3D12_RESOURCE_STATES mBufferState[2] = {};

    // Копия счётчика для шейдеров: [0] — до эмиссии, [256] — после (адреса CBV кратны 256)
    Microsoft::WRL::ComPtr<ID3D12Resource> mCountBuffer;
    // Аргументы ExecuteIndirect: { VertexCount = живые частицы, 1, 0, 0 }
    Microsoft::WRL::ComPtr<ID3D12Resource> mDrawArgs;
    // Чтобы показать число частиц в заголовке окна
    Microsoft::WRL::ComPtr<ID3D12Resource> mReadback;

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mInitUpload;

    // UAV со счётчиками: пара 0 = [buf0, buf1], пара 1 = [buf1, buf0]  (u0 = Consume, u1 = Append)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mUavHeap;
    UINT mDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mComputeRootSig;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mDrawRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mEmitPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mUpdatePSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDrawPSO;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mDrawSignature;

    std::unique_ptr<UploadBuffer<ParticleConstants>> mConstantsCB;
    ParticleConstants mConstants = {};

    int  mCurrent = 0;          // буфер A (живые частицы с прошлого кадра)
    int  mDrawBuffer = 0;       // буфер, который рисуем в этом кадре
    float mEmitRate = 15000.0f; // частиц в секунду
    float mEmitAccum = 0.0f;
    bool mEmitting = true;
    UINT mFrame = 0;
    UINT mAliveCount = 0;
};
