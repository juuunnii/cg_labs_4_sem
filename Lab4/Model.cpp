#include "Model.h"
#include <algorithm>
#include <tuple>
#include <cmath>
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
            dst.DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        }
        else
        {
            dst.DiffuseAlbedo = XMFLOAT4(src.diffuse[0], src.diffuse[1], src.diffuse[2], 1.0f);
        }

        if (!src.alpha_texname.empty())
            dst.AlphaTexture = basedir + NormalizePath(src.alpha_texname);

        if (!src.normal_texname.empty())
            dst.NormalTexture = basedir + NormalizePath(src.normal_texname);

        // Карта высот: disp, если есть, иначе bump (в Sponza — *_bump.png)
        if (!src.displacement_texname.empty())
            dst.HeightTexture = basedir + NormalizePath(src.displacement_texname);
        else if (!src.bump_texname.empty())
            dst.HeightTexture = basedir + NormalizePath(src.bump_texname);
    }

    // ---------- Геометрия, сгруппированная по материалам ----------
    const UINT defaultMat = static_cast<UINT>(materials.size());
    std::vector<std::vector<std::uint32_t>> perMaterial(materials.size() + 1);
    std::map<std::tuple<int, int, int>, std::uint32_t> uniqueVertices;

    for (const auto& shape : shapes)
    {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f)
        {
            const unsigned fv = shape.mesh.num_face_vertices[f];
            if (fv != 3) { indexOffset += fv; continue; }

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
                        vertex.TexCoord.y = 1.0f - attrib.texcoords[2 * idx.texcoord_index + 1];
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

    if (!perMaterial[defaultMat].empty())
    {
        ModelMaterial def;
        def.Name = "default";
        def.DiffuseAlbedo = XMFLOAT4(0.75f, 0.75f, 0.75f, 1.0f);
        mMaterials.push_back(def);
    }

    ComputeTangents();
    ComputeBounds();

    return !mSubsets.empty();
}

// Касательные по UV: T = dP/du, B = dP/dv (накапливаем по треугольникам, потом ортогонализуем)
void Model::ComputeTangents()
{
    std::vector<XMFLOAT3> tan(mVertices.size(), XMFLOAT3(0, 0, 0));
    std::vector<XMFLOAT3> bitan(mVertices.size(), XMFLOAT3(0, 0, 0));

    for (size_t i = 0; i + 2 < mIndices.size(); i += 3)
    {
        const std::uint32_t i0 = mIndices[i], i1 = mIndices[i + 1], i2 = mIndices[i + 2];
        const ModelVertex& v0 = mVertices[i0];
        const ModelVertex& v1 = mVertices[i1];
        const ModelVertex& v2 = mVertices[i2];

        const float e1x = v1.Position.x - v0.Position.x, e1y = v1.Position.y - v0.Position.y, e1z = v1.Position.z - v0.Position.z;
        const float e2x = v2.Position.x - v0.Position.x, e2y = v2.Position.y - v0.Position.y, e2z = v2.Position.z - v0.Position.z;
        const float du1 = v1.TexCoord.x - v0.TexCoord.x, dv1 = v1.TexCoord.y - v0.TexCoord.y;
        const float du2 = v2.TexCoord.x - v0.TexCoord.x, dv2 = v2.TexCoord.y - v0.TexCoord.y;

        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-12f)
            continue;
        const float r = 1.0f / det;

        const XMFLOAT3 T((e1x * dv2 - e2x * dv1) * r, (e1y * dv2 - e2y * dv1) * r, (e1z * dv2 - e2z * dv1) * r);
        const XMFLOAT3 B((e2x * du1 - e1x * du2) * r, (e2y * du1 - e1y * du2) * r, (e2z * du1 - e1z * du2) * r);

        for (std::uint32_t idx : { i0, i1, i2 })
        {
            tan[idx].x += T.x; tan[idx].y += T.y; tan[idx].z += T.z;
            bitan[idx].x += B.x; bitan[idx].y += B.y; bitan[idx].z += B.z;
        }
    }

    for (size_t i = 0; i < mVertices.size(); ++i)
    {
        XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&mVertices[i].Normal));
        XMVECTOR t = XMLoadFloat3(&tan[i]);

        // Грам-Шмидт: делаем T перпендикулярной N
        t = XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, t))));
        if (XMVectorGetX(XMVector3LengthSq(t)) < 1e-12f)
        {
            // Нет UV — берём любую перпендикулярную ось
            XMVECTOR axis = std::fabs(XMVectorGetY(n)) < 0.99f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
            t = XMVector3Cross(axis, n);
        }
        t = XMVector3Normalize(t);

        const float w = XMVectorGetX(XMVector3Dot(XMVector3Cross(n, t), XMLoadFloat3(&bitan[i]))) < 0.0f ? -1.0f : 1.0f;

        XMFLOAT3 t3;
        XMStoreFloat3(&t3, t);
        mVertices[i].Tangent = XMFLOAT4(t3.x, t3.y, t3.z, w);
    }
}

void Model::ComputeBounds()
{
    if (mVertices.empty())
        return;

    mBoundsMin = mBoundsMax = mVertices[0].Position;
    for (const auto& v : mVertices)
    {
        if (v.Position.x < mBoundsMin.x) mBoundsMin.x = v.Position.x;
        if (v.Position.y < mBoundsMin.y) mBoundsMin.y = v.Position.y;
        if (v.Position.z < mBoundsMin.z) mBoundsMin.z = v.Position.z;
        if (v.Position.x > mBoundsMax.x) mBoundsMax.x = v.Position.x;
        if (v.Position.y > mBoundsMax.y) mBoundsMax.y = v.Position.y;
        if (v.Position.z > mBoundsMax.z) mBoundsMax.z = v.Position.z;
    }
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