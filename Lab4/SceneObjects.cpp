#include "SceneObjects.h"
#include "Common/GeometryGenerator.h"
#include <random>
#include <cmath>

using namespace DirectX;

namespace
{
    struct SimpleVertex
    {
        XMFLOAT3 Pos;
        XMFLOAT3 Normal;
    };

    // Яркий цвет по оттенку (HSV -> RGB)
    XMFLOAT4 HueToColor(float h)
    {
        const float s = 0.75f, v = 0.95f;
        const float c = v * s;
        const float hp = h * 6.0f;
        const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
        float r = 0, g = 0, b = 0;
        if      (hp < 1) { r = c; g = x; }
        else if (hp < 2) { r = x; g = c; }
        else if (hp < 3) { g = c; b = x; }
        else if (hp < 4) { g = x; b = c; }
        else if (hp < 5) { r = x; b = c; }
        else             { r = c; b = x; }
        const float m = v - c;
        return XMFLOAT4(r + m, g + m, b + m, 1.0f);
    }
}

void SceneObjects::BuildMeshes(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
    // Все меши вписаны в куб [-0.5, 0.5]^3 — удобно для AABB
    GeometryGenerator gen;
    GeometryGenerator::MeshData meshes[MeshCount] =
    {
        gen.CreateBox(1.0f, 1.0f, 1.0f, 0),
        gen.CreateSphere(0.5f, 12, 8),
        gen.CreateGeosphere(0.5f, 1),
        gen.CreateCylinder(0.5f, 0.5f, 1.0f, 12, 1)
    };

    std::vector<SimpleVertex> vertices;
    std::vector<std::uint32_t> indices;

    for (UINT m = 0; m < MeshCount; ++m)
    {
        mMeshes[m].BaseVertexLocation = (INT)vertices.size();
        mMeshes[m].StartIndexLocation = (UINT)indices.size();
        mMeshes[m].IndexCount = (UINT)meshes[m].Indices32.size();

        for (const auto& v : meshes[m].Vertices)
            vertices.push_back({ v.Position, v.Normal });
        indices.insert(indices.end(), meshes[m].Indices32.begin(), meshes[m].Indices32.end());
    }

    mGeo = std::make_unique<MeshGeometry>();
    mGeo->Name = "objectsGeo";

    const UINT vbByteSize = (UINT)(vertices.size() * sizeof(SimpleVertex));
    const UINT ibByteSize = (UINT)(indices.size() * sizeof(std::uint32_t));

    mGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList,
        vertices.data(), vbByteSize, mGeo->VertexBufferUploader);
    mGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList,
        indices.data(), ibByteSize, mGeo->IndexBufferUploader);

    mGeo->VertexByteStride = sizeof(SimpleVertex);
    mGeo->VertexBufferByteSize = vbByteSize;
    mGeo->IndexFormat = DXGI_FORMAT_R32_UINT;
    mGeo->IndexBufferByteSize = ibByteSize;
}

void SceneObjects::Build(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                         const XMFLOAT3& sceneMin, const XMFLOAT3& sceneMax, UINT objectCount)
{
    BuildMeshes(device, cmdList);

    const XMFLOAT3 size = { sceneMax.x - sceneMin.x, sceneMax.y - sceneMin.y, sceneMax.z - sceneMin.z };
    const float longSize = (std::max)(size.x, size.z);

    // Фиксированное зерно — сцена одинаковая при каждом запуске
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);

    const BoundingBox localBox(XMFLOAT3(0, 0, 0), XMFLOAT3(0.5f, 0.5f, 0.5f));

    mObjects.resize(objectCount);
    mBounds.resize(objectCount);

    for (UINT i = 0; i < objectCount; ++i)
    {
        Object& o = mObjects[i];
        o.Mesh = rng() % MeshCount;

        const float scale = longSize * (0.004f + 0.008f * uni(rng));
        const XMFLOAT3 pos =
        {
            sceneMin.x + size.x * (0.03f + 0.94f * uni(rng)),
            sceneMin.y + size.y * (0.03f + 0.82f * uni(rng)),
            sceneMin.z + size.z * (0.03f + 0.94f * uni(rng))
        };

        XMMATRIX S = XMMatrixScaling(scale, scale, scale);
        XMMATRIX R = XMMatrixRotationRollPitchYaw(uni(rng) * XM_2PI, uni(rng) * XM_2PI, uni(rng) * XM_2PI);
        XMMATRIX T = XMMatrixTranslation(pos.x, pos.y, pos.z);
        XMMATRIX world = S * R * T;

        XMStoreFloat4x4(&o.World, world);
        o.Color = HueToColor(uni(rng));

        // Лаба 8 (PBR): каждый третий объект — металл, шероховатость случайная
        const float roughness = 0.05f + 0.9f * uni(rng);
        const float metallic = (i % 3 == 0) ? 1.0f : 0.0f;
        o.Surface = XMFLOAT4(roughness, metallic, 0.0f, 0.0f);

        // AABB объекта в мире = локальный куб, преобразованный матрицей объекта
        localBox.Transform(mBounds[i], world);
    }

    mOctree.Build(mBounds, 6, 16);

    // Буфер экземпляров:
    //   [0, N)       — все объекты (пишутся один раз, для shadow pass)
    //   [N, 2N)      — объекты, прошедшие culling (каждый кадр)
    //   [2N, ...)    — отладочные рамки узлов октодерева
    mCapacity = 2 * objectCount + kMaxDebugBoxes;
    mInstanceBuffer = std::make_unique<UploadBuffer<InstanceData>>(device, mCapacity, false);

    UINT offset = 0;
    mShadowBatches.clear();
    for (UINT m = 0; m < MeshCount; ++m)
    {
        const UINT start = offset;
        for (const Object& o : mObjects)
        {
            if (o.Mesh != m) continue;
            InstanceData d;
            XMStoreFloat4x4(&d.World, XMMatrixTranspose(XMLoadFloat4x4(&o.World)));
            d.Color = o.Color;
            d.Surface = o.Surface;
            mInstanceBuffer->CopyData((int)offset++, d);
        }
        if (offset > start)
        {
            InstancedBatch b;
            b.IndexCount = mMeshes[m].IndexCount;
            b.StartIndex = mMeshes[m].StartIndexLocation;
            b.BaseVertex = mMeshes[m].BaseVertexLocation;
            b.InstanceOffset = start;
            b.InstanceCount = offset - start;
            mShadowBatches.push_back(b);
        }
    }

    mStats.Total = objectCount;

    wchar_t msg[256];
    swprintf_s(msg, L"[Octree] objects: %u, nodes: %u, depth: %u\n",
        objectCount, mOctree.NodeCount(), mOctree.MaxDepthReached());
    OutputDebugStringW(msg);
}

void SceneObjects::Update(const BoundingFrustum& frustumW, CullingMode mode, bool showOctreeNodes)
{
    mVisible.clear();
    mVisibleNodes.clear();
    mStats.Total = (UINT)mObjects.size();
    mStats.BoxTests = 0;
    mStats.NodesVisited = 0;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    //--------------------------------------------------------------
    // Отсечение
    //--------------------------------------------------------------
    switch (mode)
    {
    case CullingMode::None:
        for (UINT i = 0; i < (UINT)mObjects.size(); ++i)
            mVisible.push_back(i);
        break;

    case CullingMode::BruteForce:
        // Каждый объект проверяется отдельно: O(N)
        for (UINT i = 0; i < (UINT)mObjects.size(); ++i)
        {
            ++mStats.BoxTests;
            if (frustumW.Intersects(mBounds[i]))
                mVisible.push_back(i);
        }
        break;

    case CullingMode::Octree:
    {
        Octree::QueryStats qs;
        mOctree.Query(frustumW, mVisible, qs, showOctreeNodes ? &mVisibleNodes : nullptr);
        mStats.BoxTests = qs.BoxTests;
        mStats.NodesVisited = qs.NodesVisited;
        break;
    }
    }

    QueryPerformanceCounter(&t1);
    mStats.TimeMs = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
    mStats.Visible = (UINT)mVisible.size();

    //--------------------------------------------------------------
    // Буфер экземпляров: видимые объекты, сгруппированные по мешу
    //--------------------------------------------------------------
    mBatches.clear();
    UINT offset = (UINT)mObjects.size();   // первая половина буфера занята статичными данными для теней

    for (UINT m = 0; m < MeshCount; ++m)
    {
        const UINT start = offset;
        for (UINT idx : mVisible)
        {
            const Object& o = mObjects[idx];
            if (o.Mesh != m)
                continue;

            InstanceData d;
            XMStoreFloat4x4(&d.World, XMMatrixTranspose(XMLoadFloat4x4(&o.World)));
            d.Color = o.Color;
            d.Surface = o.Surface;
            mInstanceBuffer->CopyData((int)offset++, d);
        }

        if (offset > start)
        {
            InstancedBatch b;
            b.IndexCount = mMeshes[m].IndexCount;
            b.StartIndex = mMeshes[m].StartIndexLocation;
            b.BaseVertex = mMeshes[m].BaseVertexLocation;
            b.InstanceOffset = start;
            b.InstanceCount = offset - start;
            mBatches.push_back(b);
        }
    }

    // Отладка: рамки узлов октодерева, попавших во фрустум
    if (showOctreeNodes && !mVisibleNodes.empty())
    {
        const UINT start = offset;
        const UINT maxBoxes = kMaxDebugBoxes;
        const UINT count = (std::min)((UINT)mVisibleNodes.size(), maxBoxes);
        for (UINT i = 0; i < count && offset < mCapacity; ++i)
        {
            const BoundingBox& nb = mVisibleNodes[i];
            XMMATRIX world = XMMatrixScaling(nb.Extents.x * 2.0f, nb.Extents.y * 2.0f, nb.Extents.z * 2.0f)
                           * XMMatrixTranslation(nb.Center.x, nb.Center.y, nb.Center.z);

            InstanceData d;
            XMStoreFloat4x4(&d.World, XMMatrixTranspose(world));
            d.Color = XMFLOAT4(1.0f, 1.0f, 0.2f, 1.0f);
            d.Surface = XMFLOAT4(1.0f, 0.0f, 0.0f, 0.0f);
            mInstanceBuffer->CopyData((int)offset++, d);
        }

        InstancedBatch b;
        b.IndexCount = mMeshes[MeshBox].IndexCount;
        b.StartIndex = mMeshes[MeshBox].StartIndexLocation;
        b.BaseVertex = mMeshes[MeshBox].BaseVertexLocation;
        b.InstanceOffset = start;
        b.InstanceCount = offset - start;
        b.Wireframe = true;
        mBatches.push_back(b);
    }
}
