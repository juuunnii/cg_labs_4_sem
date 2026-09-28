#pragma once
#include "Common/d3dUtil.h"
#include <vector>

// Октодерево для ограничивающих объёмов (AABB) объектов сцены.
//
// Каждый объект лежит в самом глубоком узле, который целиком его содержит.
// Лист делится на 8 детей, когда в нём становится больше maxObjectsPerLeaf объектов.
//
// Запрос по фрустуму:
//   узел вне фрустума      -> всё поддерево отбрасывается одной проверкой
//   узел целиком внутри    -> все объекты поддерева видимы без проверок
//   узел пересекает границу -> проверяем объекты узла и спускаемся в детей
class Octree
{
public:
    struct QueryStats
    {
        UINT NodesVisited = 0;   // сколько узлов проверили
        UINT BoxTests = 0;       // сколько всего проверок «AABB против фрустума»
    };

    void Build(const std::vector<DirectX::BoundingBox>& objectBounds,
               UINT maxDepth = 6, UINT maxObjectsPerLeaf = 16);

    // visibleNodes (необязательно) — узлы, попавшие во фрустум (для отладочной отрисовки)
    void Query(const DirectX::BoundingFrustum& frustum,
               std::vector<UINT>& visibleObjects,
               QueryStats& stats,
               std::vector<DirectX::BoundingBox>* visibleNodes = nullptr) const;

    UINT NodeCount() const { return (UINT)mNodes.size(); }
    UINT MaxDepthReached() const { return mMaxDepthReached; }

private:
    struct Node
    {
        DirectX::BoundingBox Bounds;
        int FirstChild = -1;          // 8 детей лежат в mNodes подряд; -1 — лист
        UINT Depth = 0;
        std::vector<UINT> Objects;    // объекты, которые не влезли ни в одного ребёнка
    };

    void Insert(int nodeIndex, UINT objectIndex);
    void Subdivide(int nodeIndex);
    int  ChildContaining(int nodeIndex, UINT objectIndex) const;
    void AddSubtree(int nodeIndex, std::vector<UINT>& out) const;
    void QueryNode(int nodeIndex, const DirectX::BoundingFrustum& frustum,
                   std::vector<UINT>& out, QueryStats& stats,
                   std::vector<DirectX::BoundingBox>* visibleNodes) const;

private:
    std::vector<Node> mNodes;
    std::vector<DirectX::BoundingBox> mObjectBounds;
    UINT mMaxDepth = 6;
    UINT mMaxObjectsPerLeaf = 16;
    UINT mMaxDepthReached = 0;
};
