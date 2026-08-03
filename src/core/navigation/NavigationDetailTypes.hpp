#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/vec4.hpp>

#include "core/navigation/Navigation.hpp"

namespace core::navigation_detail {

constexpr float kHorizontalNormalMinDot = 0.9848077f;
constexpr float kLayerGroupingEpsilon = 0.10f;
constexpr float kEndpointProjectionTolerance = 1.0f;
constexpr float kPolygonEpsilon = 1.0e-4f;
constexpr float kPortalBroadPhaseEpsilon = kPolygonEpsilon * 16.0f;
constexpr float kPlaneEpsilon = 1.0e-5f;
constexpr float kTau = 6.283185307179586f;
constexpr int kClearanceSampleDirections = 24;
constexpr int kSegmentClearanceSampleDirections = 8;
constexpr int kClearanceBinarySearchSteps = 10;
constexpr int kClearanceProjectionIterations = 16;
constexpr std::size_t kMaxClearanceProjectionCandidateCells = 16u;
constexpr std::size_t kMaxProjectedCellsPerDefaultHitboxPart = 16u;
constexpr std::size_t kMaxGeneratedNavMeshCells = 250000u;
constexpr int kNavAssetVersion = 1;
inline const glm::vec4 kWalkableOverlayColor(0.0f, 1.0f, 0.0f, 0.5f);

struct QuantizedVec2 {
    long long x{0};
    long long y{0};

    friend bool operator==(QuantizedVec2 lhs, QuantizedVec2 rhs) = default;
    friend bool operator<(QuantizedVec2 lhs, QuantizedVec2 rhs) {
        return lhs.x != rhs.x ? lhs.x < rhs.x : lhs.y < rhs.y;
    }
};

struct QuantizedVec2Hash {
    std::size_t operator()(const QuantizedVec2& value) const noexcept {
        return static_cast<std::size_t>(value.x * 73856093ull) ^
            static_cast<std::size_t>(value.y * 19349663ull);
    }
};

struct QuantizedLayerPoint {
    QuantizedVec2 point{};
    long long elevation{0};

    friend bool operator==(const QuantizedLayerPoint&, const QuantizedLayerPoint&) = default;
};

struct QuantizedLayerPointHash {
    std::size_t operator()(const QuantizedLayerPoint& key) const noexcept {
        std::size_t result = QuantizedVec2Hash{}(key.point);
        return result ^ (std::hash<long long>{}(key.elevation) + 0x9e3779b9u +
                         (result << 6u) + (result >> 2u));
    }
};

struct ExactLayerPoint {
    std::uint32_t x{0u};
    std::uint32_t y{0u};
    std::uint32_t z{0u};

    friend bool operator==(const ExactLayerPoint&, const ExactLayerPoint&) = default;
};

struct ExactLayerPointHash {
    std::size_t operator()(const ExactLayerPoint& key) const noexcept {
        std::size_t result = std::hash<std::uint32_t>{}(key.x);
        result ^= std::hash<std::uint32_t>{}(key.y) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
        return result ^ (std::hash<std::uint32_t>{}(key.z) + 0x9e3779b9u +
                         (result << 6u) + (result >> 2u));
    }
};

struct VisibilitySegmentKey {
    ExactLayerPoint from{};
    ExactLayerPoint to{};

    friend bool operator==(const VisibilitySegmentKey&, const VisibilitySegmentKey&) = default;
};

struct VisibilitySegmentKeyHash {
    std::size_t operator()(const VisibilitySegmentKey& key) const noexcept {
        const ExactLayerPointHash hash{};
        std::size_t result = hash(key.from);
        return result ^ (hash(key.to) + 0x9e3779b9u + (result << 6u) + (result >> 2u));
    }
};

using VisibilityTraversalCache = std::unordered_map<VisibilitySegmentKey, bool, VisibilitySegmentKeyHash>;

struct QuantizedEdge {
    QuantizedVec2 a{};
    QuantizedVec2 b{};

    friend bool operator==(const QuantizedEdge&, const QuantizedEdge&) = default;
};

struct QuantizedEdgeHash {
    std::size_t operator()(const QuantizedEdge& edge) const noexcept {
        const QuantizedVec2Hash hash{};
        return hash(edge.a) ^ (hash(edge.b) << 1u);
    }
};

struct WalkableTriangle {
    float elevationY{0.0f};
    glm::vec2 a{0.0f};
    glm::vec2 b{0.0f};
    glm::vec2 c{0.0f};
};

struct BlockingFootprint {
    float minY{0.0f};
    float maxY{0.0f};
    std::vector<glm::vec2> verticesXZ{};
    glm::vec2 minXZ{0.0f};
    glm::vec2 maxXZ{0.0f};
};

struct LayerBuildData {
    float elevationY{0.0f};
    std::vector<WalkableTriangle> triangles{};
    std::vector<BlockingFootprint> blockers{};
};

struct AuthoredBakePolygon {
    std::size_t assetIndex{0u};
    float elevationY{0.0f};
    std::vector<glm::vec2> verticesXZ{};
};

struct BakedTriangle {
    std::size_t authoredPolygonIndex{0u};
    float elevationY{0.0f};
    std::array<glm::vec2, 3u> verticesXZ{};
};

struct BakeLayerData {
    float elevationY{0.0f};
    std::vector<AuthoredBakePolygon> polygons{};
    std::vector<BakedTriangle> triangles{};
};

struct ParentPathData {
    std::unordered_map<EntityId, std::vector<EntityId>> childrenByParent{};
    std::unordered_map<EntityId, std::string> pathByEntity{};
};

struct SharedPortalResult {
    glm::vec2 a{0.0f};
    glm::vec2 b{0.0f};
};

struct NavigationSolveView {
    const NavMeshAsset& asset;
    const std::unordered_map<int, std::size_t>& polygonIndexById;
    const std::vector<glm::vec2>& polygonCenters;
    const std::vector<NavRuntimeCell>& bakedCells;
    const std::vector<glm::vec2>& bakedCellCenters;
    const std::vector<glm::vec2>& bakedCellMinXZ;
    const std::vector<glm::vec2>& bakedCellMaxXZ;
    const std::vector<std::vector<std::uint8_t>>& bakedCellBoundaryVertices;
    const std::vector<std::vector<std::size_t>>& polygonToCellIndices;
    const std::vector<std::vector<std::size_t>>& cellToPolygonIndices;
    const std::vector<std::vector<NavGraphEdge>>& graph;
    const PolyanyaMesh* polyanyaMesh;
    bool bakedCellsHaveInteriorOverlap;
};

struct SolvedPath {
    glm::vec3 destination{0.0f};
    std::vector<glm::vec3> corners{};
    glm::vec3 resolvedStart{0.0f};
};

struct ResolvedPathEndpoints {
    glm::vec3 resolvedStart{0.0f};
    glm::vec3 resolvedDestination{0.0f};
    std::vector<std::size_t> rawStartCells{};
    std::vector<std::size_t> rawTargetCells{};
    std::vector<std::size_t> startCells{};
    std::vector<std::size_t> targetCells{};
};

enum class AgentClearanceShape {
    None = 0,
    Sphere,
    Box,
};

struct AgentClearanceProfile {
    AgentClearanceShape shape{AgentClearanceShape::None};
    glm::vec2 centerXZ{0.0f};
    float sphereRadius{0.0f};
    glm::vec2 boxHalfExtentsXZ{0.0f};

    [[nodiscard]] bool empty() const { return shape == AgentClearanceShape::None; }
};

}  // namespace core::navigation_detail
