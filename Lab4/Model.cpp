#include "Model.h"
#include <algorithm>
#include <tuple>
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

using namespace DirectX;

// В .mtl Sponza пути записаны через '\' — приводим к '/' и убираем хвостовые пробелы
static std::string NormalizePath(std::string p)
{
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && (p.back() == ' ' || p.back() == '\r' || p.back() == '\t'))
        p.pop_back();
    return p;
}

bool Model::LoadFromOBJ(const std::string& filename)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    std::string basedir = filename.substr(0, filename.find_last_of("/\\") + 1);

    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
        filename.c_str(), basedir.c_str(), true);

    if (!warn.empty()) OutputDebugStringA(warn.c_str());
    if (!err.empty())  OutputDebugStringA(err.c_str());
    if (!ret) return false;

    mVertices.clear();
    mIndices.clear();
    mSubsets.clear();
    mMaterials.clear();

    // ---------- Материалы ----------
    mMaterials.resize(materials.size());
    for (size_t i = 0; i < materials.size(); ++i)
    {
        const auto& src = materials[i];
        auto& dst = mMaterials[i];
        dst.Name = src.name;

        if (!src.diffuse_texname.empty())
        {
            dst.DiffuseTexture = basedir + NormalizePath(src.diffuse_texname);
            // Цвет задаёт текстура, Kd не домножаем (иначе всё темнеет)
            dst.DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        }
        else
        {
            dst.DiffuseAlbedo = XMFLOAT4(src.diffuse[0], src.diffuse[1], src.diffuse[2], 1.0f);
        }

        if (!src.alpha_texname.empty())
            dst.AlphaTexture = basedir + NormalizePath(src.alpha_texname);
    }

    // ---------- Геометрия, сгруппированная по материалам ----------
    // Важно: один shape из tinyobj может содержать грани с разными материалами,
    // поэтому группируем по material_id каждой грани, а не по shape.
    const UINT defaultMat = static_cast<UINT>(materials.size());
    std::vector<std::vector<std::uint32_t>> perMaterial(materials.size() + 1);
    std::map<std::tuple<int, int, int>, std::uint32_t> uniqueVertices;

    for (const auto& shape : shapes)
    {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f)
        {
            const unsigned fv = shape.mesh.num_face_vertices[f];
            if (fv != 3) { indexOffset += fv; continue; } // после триангуляции должны быть только треугольники

            int mid = shape.mesh.material_ids[f];
            UINT matIndex = (mid < 0 || mid >= (int)materials.size()) ? defaultMat : (UINT)mid;

            for (unsigned v = 0; v < 3; ++v)
            {
                const tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];
                auto key = std::make_tuple(idx.vertex_index, idx.normal_index, idx.texcoord_index);

                auto it = uniqueVertices.find(key);
                std::uint32_t vertIndex;
                if (it == uniqueVertices.end())
                {
                    ModelVertex vertex;
                    vertex.Position.x = attrib.vertices[3 * idx.vertex_index + 0];
                    vertex.Position.y = attrib.vertices[3 * idx.vertex_index + 1];
                    vertex.Position.z = attrib.vertices[3 * idx.vertex_index + 2];

                    if (idx.normal_index >= 0)
                    {
                        vertex.Normal.x = attrib.normals[3 * idx.normal_index + 0];
                        vertex.Normal.y = attrib.normals[3 * idx.normal_index + 1];
                        vertex.Normal.z = attrib.normals[3 * idx.normal_index + 2];
                    }

                    if (idx.texcoord_index >= 0)
                    {
                        vertex.TexCoord.x = attrib.texcoords[2 * idx.texcoord_index + 0];
                        vertex.TexCoord.y = 1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]; // OBJ: V вверх, D3D: V вниз
                    }

                    vertIndex = static_cast<std::uint32_t>(mVertices.size());
                    uniqueVertices.emplace(key, vertIndex);
                    mVertices.push_back(vertex);
                }
                else
                {
                    vertIndex = it->second;
                }

                perMaterial[matIndex].push_back(vertIndex);
            }
            indexOffset += fv;
        }
    }

    // Склеиваем индексы в один буфер: по одному сабсету на материал
    for (UINT m = 0; m < (UINT)perMaterial.size(); ++m)
    {
        if (perMaterial[m].empty()) continue;

        ModelSubset subset;
        subset.MaterialIndex = m;
        subset.IndexStart = static_cast<UINT>(mIndices.size());
        subset.IndexCount = static_cast<UINT>(perMaterial[m].size());
        mSubsets.push_back(subset);

        mIndices.insert(mIndices.end(), perMaterial[m].begin(), perMaterial[m].end());
    }

    // Грани без материала получают материал по умолчанию (последний)
    if (!perMaterial[defaultMat].empty())
    {
        ModelMaterial def;
        def.Name = "default";
        def.DiffuseAlbedo = XMFLOAT4(0.75f, 0.75f, 0.75f, 1.0f);
        mMaterials.push_back(def);
    }

    return !mSubsets.empty();
}

void Model::CreateBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
    mMeshGeo = std::make_unique<MeshGeometry>();
    mMeshGeo->Name = "sponzaGeo";

    const UINT vbByteSize = static_cast<UINT>(mVertices.size() * sizeof(ModelVertex));
    const UINT ibByteSize = static_cast<UINT>(mIndices.size() * sizeof(std::uint32_t));

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &mMeshGeo->VertexBufferCPU));
    CopyMemory(mMeshGeo->VertexBufferCPU->GetBufferPointer(), mVertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &mMeshGeo->IndexBufferCPU));
    CopyMemory(mMeshGeo->IndexBufferCPU->GetBufferPointer(), mIndices.data(), ibByteSize);

    mMeshGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList,
        mVertices.data(), vbByteSize, mMeshGeo->VertexBufferUploader);

    mMeshGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList,
        mIndices.data(), ibByteSize, mMeshGeo->IndexBufferUploader);

    mMeshGeo->VertexByteStride = sizeof(ModelVertex);
    mMeshGeo->VertexBufferByteSize = vbByteSize;
    mMeshGeo->IndexFormat = DXGI_FORMAT_R32_UINT;
    mMeshGeo->IndexBufferByteSize = ibByteSize;

    for (size_t i = 0; i < mSubsets.size(); ++i)
    {
        SubmeshGeometry submesh;
        submesh.IndexCount = mSubsets[i].IndexCount;
        submesh.StartIndexLocation = mSubsets[i].IndexStart;
        submesh.BaseVertexLocation = 0;
        mMeshGeo->DrawArgs[mMaterials[mSubsets[i].MaterialIndex].Name] = submesh;
    }
}