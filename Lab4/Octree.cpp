#include "Octree.h"

using namespace DirectX;

void Octree::Build(const std::vector<BoundingBox>& objectBounds, UINT maxDepth, UINT maxObjectsPerLeaf)
{
    mNodes.clear();
    mObjectBounds = objectBounds;
    mMaxDepth = maxDepth;
    mMaxObjectsPerLeaf = maxObjectsPerLeaf;
    mMaxDepthReached = 0;

    if (mObjectBounds.empty())
        return;

    // Корень — куб, охватывающий все объекты (куб, чтобы дети тоже были кубами)
    BoundingBox all = mObjectBounds[0];
    for (const BoundingBox& b : mObjectBounds)
        BoundingBox::CreateMerged(all, all, b);

    const float halfSize = 1.01f * (std::max)({ all.Extents.x, all.Extents.y, all.Extents.z });

    Node root;
    root.Bounds = BoundingBox(all.Center, XMFLOAT3(halfSize, halfSize, halfSize));
    root.Depth = 0;
    mNodes.push_back(root);

    for (UINT i = 0; i < (UINT)mObjectBounds.size(); ++i)
        Insert(0, i);
}

void Octree::Subdivide(int nodeIndex)
{
    const XMFLOAT3 c = mNodes[nodeIndex].Bounds.Center;
    const XMFLOAT3 e = mNodes[nodeIndex].Bounds.Extents;
    const XMFLOAT3 he = { e.x * 0.5f, e.y * 0.5f, e.z * 0.5f };
    const UINT childDepth = mNodes[nodeIndex].Depth + 1;

    const int first = (int)mNodes.size();

    // Октант i: бит 0 — +X, бит 1 — +Y, бит 2 — +Z
    for (int i = 0; i < 8; ++i)
    {
        Node child;
        child.Bounds.Center = XMFLOAT3(
            c.x + ((i & 1) ? he.x : -he.x),
            c.y + ((i & 2) ? he.y : -he.y),
            c.z + ((i & 4) ? he.z : -he.z));
        child.Bounds.Extents = he;
        child.Depth = childDepth;
        mNodes.push_back(child);   // после push_back ссылки на mNodes недействительны — работаем по индексам
    }

    mNodes[nodeIndex].FirstChild = first;
    if (childDepth > mMaxDepthReached)
        mMaxDepthReached = childDepth;
}

// Ребёнок, который целиком содержит объект, или -1 (объект лежит на границе октантов)
int Octree::ChildContaining(int nodeIndex, UINT objectIndex) const
{
    const Node& node = mNodes[nodeIndex];
    if (node.FirstChild < 0)
        return -1;

    const BoundingBox& box = mObjectBounds[objectIndex];
    const XMFLOAT3& c = node.Bounds.Center;

    const int octant = (box.Center.x > c.x ? 1 : 0)
                     | (box.Center.y > c.y ? 2 : 0)
                     | (box.Center.z > c.z ? 4 : 0);

    const int child = node.FirstChild + octant;
    return mNodes[child].Bounds.Contains(box) == CONTAINS ? child : -1;
}

void Octree::Insert(int nodeIndex, UINT objectIndex)
{
    // Внутренний узел: спускаемся в подходящего ребёнка, если он есть
    if (mNodes[nodeIndex].FirstChild >= 0)
    {
        const int child = ChildContaining(nodeIndex, objectIndex);
        if (child >= 0)
            Insert(child, objectIndex);
        else
            mNodes[nodeIndex].Objects.push_back(objectIndex);
        return;
    }

    // Лист
    mNodes[nodeIndex].Objects.push_back(objectIndex);

    if (mNodes[nodeIndex].Objects.size() > mMaxObjectsPerLeaf &&
        mNodes[nodeIndex].Depth < mMaxDepth)
    {
        Subdivide(nodeIndex);

        // Раздаём объекты детям; те, что лежат на границе, остаются в узле
        std::vector<UINT> objects;
        objects.swap(mNodes[nodeIndex].Objects);
        for (UINT obj : objects)
        {
            const int child = ChildContaining(nodeIndex, obj);
            if (child >= 0)
                Insert(child, obj);
            else
                mNodes[nodeIndex].Objects.push_back(obj);
        }
    }
}

void Octree::AddSubtree(int nodeIndex, std::vector<UINT>& out) const
{
    const Node& node = mNodes[nodeIndex];
    out.insert(out.end(), node.Objects.begin(), node.Objects.end());

    if (node.FirstChild >= 0)
        for (int i = 0; i < 8; ++i)
            AddSubtree(node.FirstChild + i, out);
}

void Octree::QueryNode(int nodeIndex, const BoundingFrustum& frustum,
                       std::vector<UINT>& out, QueryStats& stats,
                       std::vector<BoundingBox>* visibleNodes) const
{
    const Node& node = mNodes[nodeIndex];

    ++stats.NodesVisited;
    ++stats.BoxTests;
    const ContainmentType ct = frustum.Contains(node.Bounds);

    if (ct == DISJOINT)
        return;                           // весь узел вне фрустума — пропускаем поддерево

    if (visibleNodes)
        visibleNodes->push_back(node.Bounds);

    if (ct == CONTAINS)
    {
        AddSubtree(nodeIndex, out);       // узел целиком внутри — всё видно без проверок
        return;
    }

    // Пересекает границу: проверяем собственные объекты узла...
    for (UINT obj : node.Objects)
    {
        ++stats.BoxTests;
        if (frustum.Intersects(mObjectBounds[obj]))
            out.push_back(obj);
    }

    // ...и спускаемся в детей
    if (node.FirstChild >= 0)
        for (int i = 0; i < 8; ++i)
            QueryNode(node.FirstChild + i, frustum, out, stats, visibleNodes);
}

void Octree::Query(const BoundingFrustum& frustum, std::vector<UINT>& visibleObjects,
                   QueryStats& stats, std::vector<BoundingBox>* visibleNodes) const
{
    if (mNodes.empty())
        return;
    QueryNode(0, frustum, visibleObjects, stats, visibleNodes);
}
