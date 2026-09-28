#pragma once
#include "Common/d3dUtil.h"
#include "Common/UploadBuffer.h"
#include "Octree.h"
#include "RenderingSystem.h"
#include <vector>
#include <memory>

enum class CullingMode
{
    None,         // рисуем всё
    BruteForce,   // проверяем каждый объект против фрустума
    Octree        // проверяем узлы октодерева, объекты — только в пограничных узлах
};

struct CullingStats
{
    UINT   Total = 0;          // всего объектов
    UINT   Visible = 0;        // отправлено на отрисовку
    UINT   BoxTests = 0;       // проверок «AABB против фрустума»
    UINT   NodesVisited = 0;   // узлов октодерева обойдено
    double TimeMs = 0.0;       // время отсечения на CPU
};

// Данные одного экземпляра в StructuredBuffer (совпадает с Shaders/Instanced.hlsl)
struct InstanceData
{
    DirectX::XMFLOAT4X4 World;
    DirectX::XMFLOAT4 Color;
};

// Тысячи простых объектов, раскиданных по сцене, + их отсечение по фрустуму
class SceneObjects
{
public:
    void Build(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
               const DirectX::XMFLOAT3& sceneMin, const DirectX::XMFLOAT3& sceneMax,
               UINT objectCount);

    // Раз в кадр: отсечение + заполнение буфера экземпляров + список вызовов отрисовки
    void Update(const DirectX::BoundingFrustum& frustumW, CullingMode mode, bool showOctreeNodes);

    const MeshGeometry* Geometry() const { return mGeo.get(); }
    D3D12_GPU_VIRTUAL_ADDRESS InstanceBuffer() const { return mInstanceBuffer->Resource()->GetGPUVirtualAddress(); }
    const std::vector<InstancedBatch>& Batches() const { return mBatches; }
    const std::vector<InstancedBatch>& ShadowBatches() const { return mShadowBatches; }   // все объекты — для теней
    const CullingStats& Stats() const { return mStats; }
    const Octree& Tree() const { return mOctree; }

    void DisposeUploaders() { if (mGeo) mGeo->DisposeUploaders(); }

private:
    void BuildMeshes(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);

private:
    enum MeshType : UINT { MeshBox = 0, MeshSphere, MeshGeosphere, MeshCylinder, MeshCount };

    struct Object
    {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4 Color;
        UINT Mesh = MeshBox;
    };

    static const UINT kMaxDebugBoxes = 4096;

    std::vector<Object> mObjects;
    std::vector<DirectX::BoundingBox> mBounds;    // AABB каждого объекта в мире
    Octree mOctree;

    std::unique_ptr<MeshGeometry> mGeo;
    SubmeshGeometry mMeshes[MeshCount];

    std::unique_ptr<UploadBuffer<InstanceData>> mInstanceBuffer;
    UINT mCapacity = 0;

    std::vector<UINT> mVisible;
    std::vector<DirectX::BoundingBox> mVisibleNodes;
    std::vector<InstancedBatch> mBatches;
    std::vector<InstancedBatch> mShadowBatches;
    CullingStats mStats;
};
