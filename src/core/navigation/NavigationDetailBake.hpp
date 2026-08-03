#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/navigation/NavigationDetailTypes.hpp"

namespace core::navigation_detail {

glm::vec3 transformPoint3(const glm::mat4& matrix, const glm::vec3& point);
void appendConvexPolygonToUnion(
    std::vector<NavRuntimeCell>& cells,
    std::vector<glm::vec2> polygon,
    float elevationY
);
void addTriangleToLayer(
    std::vector<LayerBuildData>& layers,
    float elevationY,
    const WalkableTriangle& triangle
);
std::vector<BlockingFootprint> buildBlockingFootprints(const World& world);
std::optional<std::vector<NavPolygon>> buildPolygonsForLayer(
    const LayerBuildData& layer,
    int& nextPolygonId,
    float maximumPolygonEdgeLength,
    float minimumTriangleArea,
    std::string* error
);
std::optional<std::vector<NavRuntimeCell>> triangulateWalkableCellsDelaunay(
    const std::vector<NavRuntimeCell>& cells,
    float elevationY,
    std::string* error
);
std::optional<std::vector<NavRuntimeCell>> triangulateWalkableCellsIndividuallyDelaunay(
    const std::vector<NavRuntimeCell>& cells,
    float elevationY,
    std::string* error
);
std::optional<std::vector<NavRuntimeCell>> enforceMaximumPolygonEdgeLength(
    const std::vector<NavRuntimeCell>& cells,
    float maximumPolygonEdgeLength
);
std::vector<NavRuntimeCell> applyMinimumTriangleAreaConstraint(
    const std::vector<NavRuntimeCell>& triangles,
    float minimumTriangleArea
);
std::vector<NavRuntimeCell> bakeLayerRuntimeCells(
    const BakeLayerData& layer,
    std::vector<std::vector<std::size_t>>& outCellToPolygonIndices
);
std::vector<NavRuntimeCell> bakeDisjointLayerRuntimeCells(
    const BakeLayerData& layer,
    std::vector<std::vector<std::size_t>>& outCellToPolygonIndices
);
void appendGraphEdgeIfMissing(
    std::vector<NavGraphEdge>& edges,
    const NavGraphEdge& candidate
);
bool bakeLayerHasInteriorPolygonOverlap(const BakeLayerData& layer);
bool runtimeCellsHaveInteriorOverlap(
    const std::vector<NavRuntimeCell>& cells,
    const std::vector<glm::vec2>& cellMinXZ,
    const std::vector<glm::vec2>& cellMaxXZ
);
void rebuildCellBoundaryVertexCache(NavigationRuntime& runtime);
void setRuntimeStatus(NavigationRuntime& runtime, std::string message, bool isError);
std::optional<std::size_t> findPolygonIndexById(
    const NavigationRuntime& runtime,
    int polygonId
);
std::optional<std::size_t> findPolygonIndexById(
    const NavMeshAsset& asset,
    int polygonId
);
glm::vec3 cellCenter3(const NavigationRuntime& runtime, std::size_t cellIndex);

}  // namespace core::navigation_detail
