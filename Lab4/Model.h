#pragma once
#include "common/d3dUtil.h"
#include <string>
#include <vector>
#include <map>
#include <memory>

struct ModelVertex
{
    DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 Normal = { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT2 TexCoord = { 0.0f, 0.0f };
};

// ћатериал из .mtl
struct ModelMaterial
{
    std::string Name;
    DirectX::XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::string DiffuseTexture;   // map_Kd (путь уже с папкой модели, пусто = нет текстуры)
    std::string AlphaTexture;     // map_d  (маска прозрачности: листь€, цепи)
};

//  усок индексного буфера, который рисуетс€ одним материалом
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

private:
    std::vector<ModelVertex>   mVertices;
    std::vector<std::uint32_t> mIndices;
    std::vector<ModelSubset>   mSubsets;
    std::vector<ModelMaterial> mMaterials;
    std::unique_ptr<MeshGeometry> mMeshGeo;
};