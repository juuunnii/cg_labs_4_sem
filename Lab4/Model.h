#pragma once
#include "common/d3dUtil.h"
#include <string>
#include <vector>
#include <map>
#include <memory>

// Вершина: позиция, нормаль, UV и касательная (для normal mapping).
// Tangent.w = ±1 — «рукость» базиса: B = w * cross(N, T)
struct ModelVertex
{
    DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 Normal = { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT2 TexCoord = { 0.0f, 0.0f };
    DirectX::XMFLOAT4 Tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
};

// Материал из .mtl
struct ModelMaterial
{
    std::string Name;
    DirectX::XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::string DiffuseTexture;   // map_Kd
    std::string AlphaTexture;     // map_d   — маска прозрачности
    std::string NormalTexture;    // norm    — готовая карта нормалей (если есть)
    std::string HeightTexture;    // disp / map_bump / bump — карта высот для displacement
};

// Кусок индексного буфера, рисуемый одним материалом
struct ModelSubset
{
    UINT MaterialIndex = 0;
    UINT IndexStart = 0;
    UINT IndexCount = 0;
};

class Model
{
public:
    bool LoadFromOBJ(const std::string& filename);
    void CreateBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);

    std::unique_ptr<MeshGeometry> GetMeshGeometry() { return std::move(mMeshGeo); }
    const std::vector<ModelSubset>& GetSubsets()   const { return mSubsets; }
    const std::vector<ModelMaterial>& GetMaterials() const { return mMaterials; }

    DirectX::XMFLOAT3 GetBoundsMin() const { return mBoundsMin; }
    DirectX::XMFLOAT3 GetBoundsMax() const { return mBoundsMax; }

private:
    void ComputeTangents();
    void ComputeBounds();

private:
    DirectX::XMFLOAT3 mBoundsMin = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 mBoundsMax = { 0.0f, 0.0f, 0.0f };

    std::vector<ModelVertex>   mVertices;
    std::vector<std::uint32_t> mIndices;
    std::vector<ModelSubset>   mSubsets;
    std::vector<ModelMaterial> mMaterials;
    std::unique_ptr<MeshGeometry> mMeshGeo;
};