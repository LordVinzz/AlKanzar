#include "Navigation.hpp"
#include "Polyanya.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <SDL.h>
#include <glm/geometric.hpp>

#include "core/profiling/ProfilerService.hpp"
#include "core/systems/PickingSystem.hpp"
#include "core/transform/TransformMath.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace core {

namespace {

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
constexpr int kNavAssetVersion = 1;
const glm::vec4 kWalkableOverlayColor(0.0f, 1.0f, 0.0f, 0.5f);

struct QuantizedVec2 {
    long long x{0};
    long long y{0};

    friend bool operator==(QuantizedVec2 lhs, QuantizedVec2 rhs) = default;
    friend bool operator<(QuantizedVec2 lhs, QuantizedVec2 rhs) {
        if (lhs.x != rhs.x) {
            return lhs.x < rhs.x;
        }
        return lhs.y < rhs.y;
    }
};

struct QuantizedVec2Hash {
    std::size_t operator()(const QuantizedVec2& value) const noexcept {
        return static_cast<std::size_t>(value.x * 73856093ull) ^ static_cast<std::size_t>(value.y * 19349663ull);
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
        result ^= std::hash<long long>{}(key.elevation) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
        return result;
    }
};

struct ExactLayerPoint {
    std::uint32_t x{0u};
    std::uint32_t y{0u};
    std::uint32_t z{0u};

    friend bool operator==(const ExactLayerPoint&, const ExactLayerPoint&) =
        default;
};

struct ExactLayerPointHash {
    std::size_t operator()(const ExactLayerPoint& key) const noexcept {
        std::size_t result = std::hash<std::uint32_t>{}(key.x);
        result ^= std::hash<std::uint32_t>{}(key.y) + 0x9e3779b9u +
            (result << 6u) + (result >> 2u);
        result ^= std::hash<std::uint32_t>{}(key.z) + 0x9e3779b9u +
            (result << 6u) + (result >> 2u);
        return result;
    }
};

struct VisibilitySegmentKey {
    ExactLayerPoint from{};
    ExactLayerPoint to{};

    friend bool operator==(
        const VisibilitySegmentKey&,
        const VisibilitySegmentKey&
    ) = default;
};

struct VisibilitySegmentKeyHash {
    std::size_t operator()(const VisibilitySegmentKey& key) const noexcept {
        const ExactLayerPointHash hash{};
        std::size_t result = hash(key.from);
        result ^= hash(key.to) + 0x9e3779b9u +
            (result << 6u) + (result >> 2u);
        return result;
    }
};

using VisibilityTraversalCache = std::unordered_map<
    VisibilitySegmentKey,
    bool,
    VisibilitySegmentKeyHash
>;

struct QuantizedEdge {
    QuantizedVec2 a{};
    QuantizedVec2 b{};

    friend bool operator==(const QuantizedEdge&, const QuantizedEdge&) = default;
};

struct QuantizedEdgeHash {
    std::size_t operator()(const QuantizedEdge& edge) const noexcept {
        QuantizedVec2Hash hash{};
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
    const navigation_detail::PolyanyaMesh* polyanyaMesh;
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

    [[nodiscard]] bool empty() const {
        return shape == AgentClearanceShape::None;
    }
};

glm::vec3 cellCenter3(const NavigationSolveView& runtime, std::size_t cellIndex);
std::vector<glm::vec2> clipConvexPolygonAgainstHalfPlane(
    const std::vector<glm::vec2>& polygon,
    const glm::vec2& lineA,
    const glm::vec2& lineB,
    bool keepLeft,
    float tolerance = kPolygonEpsilon
);
std::string canonicalPolygonKey(const std::vector<glm::vec2>& vertices);
std::vector<glm::vec2> buildConvexHull(std::vector<glm::vec2> points);
void mergeAdjacentConvexCells(std::vector<NavRuntimeCell>& cells);
glm::vec2 closestPointOnPolygonXZ(
    const glm::vec2& point,
    const std::vector<glm::vec2>& polygon
);
bool pointInOrOnPolygonXZ(
    const glm::vec2& point,
    const std::vector<glm::vec2>& polygon
);

float cross2(const glm::vec2& lhs, const glm::vec2& rhs) {
    return lhs.x * rhs.y - lhs.y * rhs.x;
}

float triArea2(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    return cross2(b - a, c - a);
}

double preciseTriArea2(
    const glm::vec2& a,
    const glm::vec2& b,
    const glm::vec2& c
) {
    const double abX = static_cast<double>(b.x) - a.x;
    const double abY = static_cast<double>(b.y) - a.y;
    const double acX = static_cast<double>(c.x) - a.x;
    const double acY = static_cast<double>(c.y) - a.y;
    return abX * acY - abY * acX;
}

double funnelArea2(
    const glm::vec2& apex,
    const glm::vec2& side,
    const glm::vec2& candidate
) {
    const double sideX = static_cast<double>(side.x) - apex.x;
    const double sideY = static_cast<double>(side.y) - apex.y;
    const double candidateX = static_cast<double>(candidate.x) - apex.x;
    const double candidateY = static_cast<double>(candidate.y) - apex.y;
    return sideX * candidateY - sideY * candidateX;
}

double funnelAreaTolerance(
    const glm::vec2& apex,
    const glm::vec2& side,
    const glm::vec2& candidate
) {
    const double sideLength = glm::distance(
        glm::dvec2(apex),
        glm::dvec2(side));
    const double candidateLength = glm::distance(
        glm::dvec2(apex),
        glm::dvec2(candidate));
    return 1.0e-10 * std::max(1.0, sideLength * candidateLength);
}

bool nearlyEqual(float lhs, float rhs, float epsilon = kPolygonEpsilon) {
    return std::abs(lhs - rhs) <= epsilon;
}

bool nearlyEqualVec2(const glm::vec2& lhs, const glm::vec2& rhs, float epsilon = kPolygonEpsilon) {
    return glm::length(lhs - rhs) <= epsilon;
}

bool nearlyEqualVec3(const glm::vec3& lhs, const glm::vec3& rhs, float epsilon = kPolygonEpsilon) {
    return glm::length(lhs - rhs) <= epsilon;
}

QuantizedVec2 quantizeVec2(const glm::vec2& value) {
    return QuantizedVec2{
        static_cast<long long>(std::llround(static_cast<double>(value.x) * 10000.0)),
        static_cast<long long>(std::llround(static_cast<double>(value.y) * 10000.0))
    };
}

QuantizedLayerPoint quantizeLayerPoint(const glm::vec2& point, float elevation) {
    return QuantizedLayerPoint{
        quantizeVec2(point),
        static_cast<long long>(std::llround(static_cast<double>(elevation) * 10000.0))
    };
}

ExactLayerPoint exactLayerPoint(const glm::vec3& point) {
    return ExactLayerPoint{
        std::bit_cast<std::uint32_t>(point.x),
        std::bit_cast<std::uint32_t>(point.y),
        std::bit_cast<std::uint32_t>(point.z),
    };
}

std::string lowercaseCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string entityName(const World& world, EntityId entity) {
    if (const NameComponent* name = world.names.tryGet(entity)) {
        return name->value;
    }
    return "Entity " + std::to_string(entity.index);
}

float planarAbsMax(const TransformComponent& transform) {
    return std::max(std::abs(transform.scale.x), std::abs(transform.scale.z));
}

glm::vec2 normalizeOrFallback(const glm::vec2& vector, const glm::vec2& fallback = glm::vec2(0.0f, 1.0f)) {
    const float length = glm::length(vector);
    return length > kPlaneEpsilon ? vector / length : fallback;
}

glm::vec2 rotateLocalXZToPlanar(const glm::vec2& local, const glm::vec2& forward) {
    const glm::vec2 safeForward = normalizeOrFallback(forward);
    const glm::vec2 right(safeForward.y, -safeForward.x);
    return right * local.x + safeForward * local.y;
}

float supportDistance(const AgentClearanceProfile& profile, const glm::vec2& sampleDirection, const glm::vec2& travelDirection) {
    if (profile.empty()) {
        return 0.0f;
    }

    const glm::vec2 direction = normalizeOrFallback(sampleDirection);
    if (profile.shape == AgentClearanceShape::Sphere) {
        const glm::vec2 center = rotateLocalXZToPlanar(profile.centerXZ, travelDirection);
        return std::abs(glm::dot(direction, center)) + profile.sphereRadius;
    }

    const glm::vec2 forward = normalizeOrFallback(travelDirection);
    const glm::vec2 right(forward.y, -forward.x);
    const glm::vec2 center = rotateLocalXZToPlanar(profile.centerXZ, travelDirection);
    return std::abs(glm::dot(direction, center)) +
        std::abs(glm::dot(direction, right)) * profile.boxHalfExtentsXZ.x +
        std::abs(glm::dot(direction, forward)) * profile.boxHalfExtentsXZ.y;
}

template <typename Visitor>
bool visitClearanceBoundaryOffsets(
    const AgentClearanceProfile& profile,
    const glm::vec2& travelDirection,
    int sphereSampleDirections,
    Visitor&& visitor
) {
    if (profile.empty()) {
        return true;
    }

    const glm::vec2 forward = normalizeOrFallback(travelDirection);
    const glm::vec2 right(forward.y, -forward.x);
    const glm::vec2 center =
        rotateLocalXZToPlanar(profile.centerXZ, travelDirection);
    if (profile.shape == AgentClearanceShape::Sphere) {
        for (int sampleIndex = 0;
             sampleIndex < sphereSampleDirections;
             ++sampleIndex) {
            const float angle = kTau * static_cast<float>(sampleIndex) /
                static_cast<float>(sphereSampleDirections);
            const glm::vec2 offset = center +
                glm::vec2(std::cos(angle), std::sin(angle)) *
                    profile.sphereRadius;
            if (!visitor(offset)) {
                return false;
            }
        }
        return true;
    }

    const glm::vec2 lateral = right * profile.boxHalfExtentsXZ.x;
    const glm::vec2 longitudinal = forward * profile.boxHalfExtentsXZ.y;
    const std::array<glm::vec2, 4u> offsets{
        center + lateral + longitudinal,
        center + lateral - longitudinal,
        center - lateral + longitudinal,
        center - lateral - longitudinal,
    };
    for (const glm::vec2& offset : offsets) {
        if (!visitor(offset)) {
            return false;
        }
    }
    return true;
}

AgentClearanceProfile resolveAgentClearanceProfile(const World& world, EntityId entity, const NavAgentComponent& agent) {
    if (agent.clearanceSource == NavAgentClearanceSource::None) {
        return AgentClearanceProfile{};
    }

    const TransformComponent* transform = world.transforms.tryGet(entity);
    if (transform == nullptr) {
        return AgentClearanceProfile{};
    }

    const SphereColliderComponent* sphere = world.sphereColliders.tryGet(entity);
    const BoxColliderComponent* box = world.boxColliders.tryGet(entity);
    switch (agent.clearanceSource) {
        case NavAgentClearanceSource::SphereCollider:
            if (sphere != nullptr) {
                return AgentClearanceProfile{
                    AgentClearanceShape::Sphere,
                    glm::vec2(sphere->center.x * transform->scale.x, sphere->center.z * transform->scale.z),
                    sphere->radius * planarAbsMax(*transform),
                    glm::vec2(0.0f)
                };
            }
            return AgentClearanceProfile{};
        case NavAgentClearanceSource::BoxCollider:
            if (box != nullptr) {
                return AgentClearanceProfile{
                    AgentClearanceShape::Box,
                    glm::vec2(box->center.x * transform->scale.x, box->center.z * transform->scale.z),
                    0.0f,
                    glm::vec2(
                        std::abs(box->halfExtents.x * transform->scale.x),
                        std::abs(box->halfExtents.z * transform->scale.z)
                    )
                };
            }
            return AgentClearanceProfile{};
        case NavAgentClearanceSource::Auto:
            if (sphere != nullptr) {
                return AgentClearanceProfile{
                    AgentClearanceShape::Sphere,
                    glm::vec2(sphere->center.x * transform->scale.x, sphere->center.z * transform->scale.z),
                    sphere->radius * planarAbsMax(*transform),
                    glm::vec2(0.0f)
                };
            }
            if (box != nullptr) {
                return AgentClearanceProfile{
                    AgentClearanceShape::Box,
                    glm::vec2(box->center.x * transform->scale.x, box->center.z * transform->scale.z),
                    0.0f,
                    glm::vec2(
                        std::abs(box->halfExtents.x * transform->scale.x),
                        std::abs(box->halfExtents.z * transform->scale.z)
                    )
                };
            }
            return AgentClearanceProfile{};
        case NavAgentClearanceSource::None:
        default:
            return AgentClearanceProfile{};
    }
}

glm::vec2 travelDirectionForSegment(const glm::vec3& from, const glm::vec3& to, const glm::vec2& fallback = glm::vec2(0.0f, 1.0f)) {
    return normalizeOrFallback(glm::vec2(to.x - from.x, to.z - from.z), fallback);
}

float conservativeClearanceRadius(const AgentClearanceProfile& profile) {
    if (profile.shape == AgentClearanceShape::Sphere) {
        return glm::length(profile.centerXZ) + profile.sphereRadius;
    }
    if (profile.shape == AgentClearanceShape::Box) {
        return glm::length(profile.centerXZ) + glm::length(profile.boxHalfExtentsXZ);
    }
    return 0.0f;
}

AgentClearanceProfile headingIndependentNodeClearance(
    const AgentClearanceProfile& profile
) {
    if (profile.empty()) {
        return {};
    }
    const float minimumExtent =
        profile.shape == AgentClearanceShape::Sphere
        ? profile.sphereRadius
        : std::min(
            profile.boxHalfExtentsXZ.x,
            profile.boxHalfExtentsXZ.y
        );
    const float inscribedRadius = std::max(
        0.0f,
        minimumExtent - glm::length(profile.centerXZ)
    );
    if (inscribedRadius <= kPolygonEpsilon) {
        return {};
    }
    return AgentClearanceProfile{
        AgentClearanceShape::Sphere,
        glm::vec2(0.0f),
        inscribedRadius,
        glm::vec2(0.0f)
    };
}

std::optional<SharedPortalResult> shrinkPortal(
    const glm::vec2& a,
    const glm::vec2& b,
    const AgentClearanceProfile& profile,
    const glm::vec2& travelDirection
) {
    if (profile.empty()) {
        return SharedPortalResult{a, b};
    }

    const glm::vec2 delta = b - a;
    const float length = glm::length(delta);
    if (length <= kPolygonEpsilon) {
        return std::nullopt;
    }
    const glm::vec2 direction = delta / length;
    const glm::vec2 forward = normalizeOrFallback(travelDirection);
    const glm::vec2 right(forward.y, -forward.x);
    const glm::vec2 center =
        rotateLocalXZToPlanar(profile.centerXZ, forward);
    const float centerProjection = glm::dot(direction, center);
    const float symmetricExtent =
        profile.shape == AgentClearanceShape::Sphere
        ? profile.sphereRadius
        : std::abs(glm::dot(direction, right)) *
                profile.boxHalfExtentsXZ.x +
            std::abs(glm::dot(direction, forward)) *
                profile.boxHalfExtentsXZ.y;
    // The collider center may be offset from the agent origin. Its feasible
    // origin interval is therefore asymmetric along the portal.
    const float startInset = std::max(
        0.0f,
        symmetricExtent - centerProjection
    );
    const float endInset = std::max(
        0.0f,
        symmetricExtent + centerProjection
    );
    if (length <= startInset + endInset + kPolygonEpsilon) {
        return std::nullopt;
    }
    return SharedPortalResult{
        a + direction * startInset,
        b - direction * endInset
    };
}

bool portalIsInternalToSharedAuthoredPolygon(
    const NavigationSolveView& runtime,
    std::size_t cellA,
    std::size_t cellB,
    const SharedPortalResult& portal
) {
    if (cellA >= runtime.cellToPolygonIndices.size() || cellB >= runtime.cellToPolygonIndices.size()) {
        return false;
    }
    const glm::vec2 portalMidpoint = (portal.a + portal.b) * 0.5f;
    const std::vector<std::size_t>& polyA = runtime.cellToPolygonIndices[cellA];
    const std::vector<std::size_t>& polyB = runtime.cellToPolygonIndices[cellB];
    for (std::size_t indexA : polyA) {
        for (std::size_t indexB : polyB) {
            if (indexA != indexB ||
                indexA >= runtime.asset.polygons.size()) {
                continue;
            }
            const std::vector<glm::vec2>& authoredVertices =
                runtime.asset.polygons[indexA].verticesXZ;
            if (pointInOrOnPolygonXZ(
                    portalMidpoint,
                    authoredVertices)) {
                return true;
            }
        }
    }
    return false;
}

// Forward declarations for functions defined later in the file
bool pointInsideAuthoredWalkableSurface(const NavigationSolveView& runtime, const glm::vec3& point);
bool segmentInsideAuthoredWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to
);
bool boxSweepInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection,
    const std::vector<std::size_t>* candidateCells = nullptr
);
bool sphereSweepInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection,
    const std::vector<std::size_t>* candidateCells = nullptr
);
template <typename RuntimeView>
bool segmentInsideBakedWalkableSurface(
    const RuntimeView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const std::vector<std::size_t>* candidateCells = nullptr
);
std::optional<std::size_t> findNearestCell(const NavigationSolveView& runtime, const glm::vec3& point);

glm::vec2 clearanceSampleDirection(int sampleIndex, int sampleCount = kClearanceSampleDirections) {
    const float angle = kTau * static_cast<float>(sampleIndex) / static_cast<float>(sampleCount);
    return glm::vec2(std::cos(angle), std::sin(angle));
}

bool pointInsideAuthoredWalkableSurfaceWithClearanceSamples(
    const NavigationSolveView& runtime,
    const glm::vec3& point,
    const AgentClearanceProfile& profile,
    const glm::vec2& travelDirection,
    int sampleDirections
) {
    if (!pointInsideAuthoredWalkableSurface(runtime, point)) {
        return false;
    }
    if (profile.empty()) {
        return true;
    }
    if (profile.shape == AgentClearanceShape::Box) {
        return boxSweepInsideWalkableSurface(
            runtime,
            point,
            point,
            profile,
            travelDirection
        );
    }
    if (!visitClearanceBoundaryOffsets(
            profile,
            travelDirection,
            sampleDirections,
            [&](const glm::vec2& offset) {
                const glm::vec3 samplePoint(
                    point.x + offset.x,
                    point.y,
                    point.z + offset.y
                );
                return pointInsideAuthoredWalkableSurface(
                    runtime,
                    samplePoint
                );
            })) {
        return false;
    }
    return sphereSweepInsideWalkableSurface(
        runtime,
        point,
        point,
        profile,
        travelDirection
    );
}

bool pointInsideAuthoredWalkableSurfaceWithClearance(
    const NavigationSolveView& runtime,
    const glm::vec3& point,
    const AgentClearanceProfile& profile,
    const glm::vec2& travelDirection
) {
    return pointInsideAuthoredWalkableSurfaceWithClearanceSamples(
        runtime,
        point,
        profile,
        travelDirection,
        kClearanceSampleDirections
    );
}

std::optional<glm::vec3> resolvePointWithClearance(
    const NavigationSolveView& runtime,
    const glm::vec3& point,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection = glm::vec2(0.0f, 1.0f),
    const std::atomic<bool>* cancelled = nullptr
) {
    const auto isCancelled = [&]() {
        return cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed);
    };
    if (isCancelled()) {
        return std::nullopt;
    }
    if (profile.empty()) {
        return pointInsideAuthoredWalkableSurface(runtime, point)
            ? std::optional<glm::vec3>(point)
            : std::optional<glm::vec3>{};
    }
    if (pointInsideAuthoredWalkableSurfaceWithClearance(runtime, point, profile, preferredTravelDirection)) {
        return point;
    }

    glm::vec3 resolved = point;
    if (!pointInsideAuthoredWalkableSurface(runtime, resolved)) {
        const std::optional<std::size_t> nearest = findNearestCell(runtime, point);
        if (!nearest.has_value()) {
            return std::nullopt;
        }
        const NavRuntimeCell& nearestCell = runtime.bakedCells[*nearest];
        const glm::vec2 nearestPoint = closestPointOnPolygonXZ(
            glm::vec2(point.x, point.z),
            nearestCell.verticesXZ
        );
        resolved = glm::vec3(
            nearestPoint.x,
            nearestCell.elevationY,
            nearestPoint.y
        );
    }
    const glm::vec2 inwardDirection = travelDirectionForSegment(point, resolved, preferredTravelDirection);
    if (pointInsideAuthoredWalkableSurfaceWithClearance(runtime, resolved, profile, inwardDirection)) {
        return resolved;
    }

    for (int iteration = 0; iteration < kClearanceProjectionIterations; ++iteration) {
        if (isCancelled()) {
            return std::nullopt;
        }
        glm::vec2 correction(0.0f);
        bool anyOutside = false;
        bool cancelledDuringSamples = false;
        int boundarySampleCount = 0;
        visitClearanceBoundaryOffsets(
            profile,
            inwardDirection,
            kClearanceSampleDirections,
            [&](const glm::vec2& offset) {
            if (isCancelled()) {
                cancelledDuringSamples = true;
                return false;
            }
            ++boundarySampleCount;
            const glm::vec3 samplePoint(
                resolved.x + offset.x,
                resolved.y,
                resolved.z + offset.y
            );
            if (pointInsideAuthoredWalkableSurface(runtime, samplePoint)) {
                return true;
            }

            anyOutside = true;
            const float clearanceDistance = glm::length(offset);
            if (clearanceDistance <= kPlaneEpsilon) {
                return true;
            }
            const glm::vec2 direction = offset / clearanceDistance;
            float insideT = 0.0f;
            float outsideT = 1.0f;
            for (int searchStep = 0; searchStep < kClearanceBinarySearchSteps; ++searchStep) {
                const float midT = (insideT + outsideT) * 0.5f;
                const glm::vec3 midPoint(
                    resolved.x + direction.x * clearanceDistance * midT,
                    resolved.y,
                    resolved.z + direction.y * clearanceDistance * midT
                );
                if (pointInsideAuthoredWalkableSurface(runtime, midPoint)) {
                    insideT = midT;
                } else {
                    outsideT = midT;
                }
            }
            correction -= direction * (clearanceDistance * (1.0f - insideT));
            return true;
        });
        if (cancelledDuringSamples) {
            return std::nullopt;
        }

        if (!anyOutside) {
            return resolved;
        }

        const float correctionLength = glm::length(correction);
        if (correctionLength <= kPolygonEpsilon) {
            break;
        }

        const glm::vec2 delta = correction /
            static_cast<float>(std::max(boundarySampleCount, 1));
        bool advanced = false;
        float stepScale = 1.0f;
        while (stepScale >= 0.125f) {
            const glm::vec3 candidate(
                resolved.x + delta.x * stepScale,
                resolved.y,
                resolved.z + delta.y * stepScale
            );
            if (pointInsideAuthoredWalkableSurface(runtime, candidate)) {
                resolved = candidate;
                advanced = true;
                break;
            }
            stepScale *= 0.5f;
        }
        if (!advanced) {
            break;
        }
        if (pointInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                resolved,
                profile,
                travelDirectionForSegment(point, resolved, preferredTravelDirection))) {
            return resolved;
        }
    }

    std::optional<glm::vec3> bestCandidate{};
    float bestDistance = std::numeric_limits<float>::max();
    const auto considerCandidate = [&](const glm::vec3& candidate) {
        if (!pointInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                candidate,
                profile,
                travelDirectionForSegment(point, candidate, preferredTravelDirection))) {
            return;
        }
        const float distance = glm::distance(point, candidate);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestCandidate = candidate;
        }
    };
    const auto considerProjectedCandidate = [&](const glm::vec3& safeTarget) {
        if (!pointInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                safeTarget,
                profile,
                travelDirectionForSegment(point, safeTarget, preferredTravelDirection))) {
            return;
        }
        float unsafeT = 0.0f;
        float safeT = 1.0f;
        for (int searchStep = 0; searchStep < kClearanceBinarySearchSteps; ++searchStep) {
            const float midT = (unsafeT + safeT) * 0.5f;
            const glm::vec3 midPoint = point + (safeTarget - point) * midT;
            if (pointInsideAuthoredWalkableSurfaceWithClearance(
                    runtime,
                    midPoint,
                    profile,
                    travelDirectionForSegment(point, midPoint, preferredTravelDirection))) {
                safeT = midT;
            } else {
                unsafeT = midT;
            }
        }
        considerCandidate(point + (safeTarget - point) * safeT);
    };

    considerCandidate(resolved);
    considerProjectedCandidate(resolved);

    std::vector<std::pair<float, std::size_t>> nearestCells{};
    nearestCells.reserve(runtime.bakedCells.size());
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t cellIndex = 0u;
         cellIndex < runtime.bakedCells.size();
         ++cellIndex) {
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        const glm::vec2 closest =
            closestPointOnPolygonXZ(pointXZ, cell.verticesXZ);
        const float score =
            glm::distance(pointXZ, closest) +
            std::abs(point.y - cell.elevationY) * 2.0f;
        nearestCells.emplace_back(score, cellIndex);
    }
    const std::size_t candidateCellCount = std::min(
        nearestCells.size(),
        kMaxClearanceProjectionCandidateCells
    );
    std::partial_sort(
        nearestCells.begin(),
        nearestCells.begin() +
            static_cast<std::ptrdiff_t>(candidateCellCount),
        nearestCells.end()
    );
    for (std::size_t candidateIndex = 0u;
         candidateIndex < candidateCellCount;
         ++candidateIndex) {
        if (isCancelled()) {
            return std::nullopt;
        }
        const std::size_t cellIndex = nearestCells[candidateIndex].second;
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        const glm::vec3 cellCenter = cellCenter3(runtime, cellIndex);
        const glm::vec2 closest =
            closestPointOnPolygonXZ(pointXZ, cell.verticesXZ);
        const glm::vec2 centerXZ(cellCenter.x, cellCenter.z);
        const glm::vec2 towardCenter = centerXZ - closest;
        const float distanceToCenter = glm::length(towardCenter);
        if (distanceToCenter > kPlaneEpsilon) {
            const float insetDistance = std::min(
                distanceToCenter,
                std::max(
                    conservativeClearanceRadius(profile) * 1.25f,
                    0.05f
                )
            );
            const glm::vec2 inset =
                closest + towardCenter / distanceToCenter * insetDistance;
            const glm::vec3 insetPoint(
                inset.x,
                cell.elevationY,
                inset.y
            );
            considerCandidate(insetPoint);
            considerProjectedCandidate(insetPoint);
        }
        considerCandidate(cellCenter);
        considerProjectedCandidate(cellCenter);
    }

    const float nominalClearance = std::max(
        supportDistance(profile, glm::vec2(1.0f, 0.0f), preferredTravelDirection),
        supportDistance(profile, glm::vec2(0.0f, 1.0f), preferredTravelDirection)
    );
    float searchLimit = std::max(nominalClearance * 4.0f, 1.0f);
    if (const std::optional<std::size_t> nearest = findNearestCell(runtime, point); nearest.has_value()) {
        const NavRuntimeCell& cell = runtime.bakedCells[*nearest];
        const glm::vec2 closest =
            closestPointOnPolygonXZ(pointXZ, cell.verticesXZ);
        const glm::vec3 closestPoint(
            closest.x,
            cell.elevationY,
            closest.y
        );
        searchLimit = std::max(
            searchLimit,
            glm::distance(point, closestPoint) +
                nominalClearance * 2.0f
        );
    }

    const float radiusStep = std::max(nominalClearance * 0.25f, 0.05f);
    for (float radius = radiusStep; radius <= searchLimit; radius += radiusStep) {
        if (isCancelled()) {
            return std::nullopt;
        }
        for (int sampleIndex = 0; sampleIndex < kClearanceSampleDirections; ++sampleIndex) {
            const glm::vec2 direction = clearanceSampleDirection(sampleIndex);
            considerCandidate(glm::vec3(
                point.x + direction.x * radius,
                resolved.y,
                point.z + direction.y * radius
            ));
        }
    }
    return bestCandidate;
}

std::optional<glm::vec3> projectEndpointOntoWalkableLayer(
    const NavigationSolveView& runtime,
    const glm::vec3& point
) {
    const glm::vec2 pointXZ(point.x, point.z);
    std::optional<glm::vec3> projected{};
    float nearestVerticalDistance = kEndpointProjectionTolerance +
        kPolygonEpsilon;
    for (std::size_t cellIndex = 0u;
         cellIndex < runtime.bakedCells.size();
         ++cellIndex) {
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        const float verticalDistance =
            std::abs(point.y - cell.elevationY);
        if (verticalDistance > nearestVerticalDistance) {
            continue;
        }
        if (cellIndex < runtime.bakedCellMinXZ.size() &&
            cellIndex < runtime.bakedCellMaxXZ.size() &&
            (pointXZ.x < runtime.bakedCellMinXZ[cellIndex].x -
                    kPolygonEpsilon ||
             pointXZ.x > runtime.bakedCellMaxXZ[cellIndex].x +
                    kPolygonEpsilon ||
             pointXZ.y < runtime.bakedCellMinXZ[cellIndex].y -
                    kPolygonEpsilon ||
             pointXZ.y > runtime.bakedCellMaxXZ[cellIndex].y +
                    kPolygonEpsilon)) {
            continue;
        }
        if (!pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            continue;
        }
        nearestVerticalDistance = verticalDistance;
        projected = glm::vec3(point.x, cell.elevationY, point.z);
    }
    return projected;
}

NavigationSolveView makeSolveView(const NavigationRuntime& runtime) {
    return NavigationSolveView{
        runtime.asset,
        runtime.polygonIndexById,
        runtime.polygonCenters,
        runtime.bakedCells,
        runtime.bakedCellCenters,
        runtime.bakedCellMinXZ,
        runtime.bakedCellMaxXZ,
        runtime.bakedCellBoundaryVertices,
        runtime.polygonToCellIndices,
        runtime.cellToPolygonIndices,
        runtime.graph,
        runtime.polyanyaMesh.get(),
        runtime.bakedCellsHaveInteriorOverlap
    };
}

NavigationSolveView makeSolveView(const NavigationSolveSnapshot& snapshot) {
    return NavigationSolveView{
        snapshot.asset,
        snapshot.polygonIndexById,
        snapshot.polygonCenters,
        snapshot.bakedCells,
        snapshot.bakedCellCenters,
        snapshot.bakedCellMinXZ,
        snapshot.bakedCellMaxXZ,
        snapshot.bakedCellBoundaryVertices,
        snapshot.polygonToCellIndices,
        snapshot.cellToPolygonIndices,
        snapshot.graph,
        snapshot.polyanyaMesh.get(),
        snapshot.bakedCellsHaveInteriorOverlap
    };
}

std::shared_ptr<const NavigationSolveSnapshot> buildSolveSnapshot(const NavigationRuntime& runtime) {
    return std::make_shared<const NavigationSolveSnapshot>(NavigationSolveSnapshot{
        runtime.asset,
        runtime.polygonIndexById,
        runtime.polygonCenters,
        runtime.bakedCells,
        runtime.bakedCellCenters,
        runtime.bakedCellMinXZ,
        runtime.bakedCellMaxXZ,
        runtime.bakedCellBoundaryVertices,
        runtime.polygonToCellIndices,
        runtime.cellToPolygonIndices,
        runtime.graph,
        runtime.polyanyaMesh,
        runtime.bakedCellsHaveInteriorOverlap
    });
}

void appendPathCorner(std::vector<glm::vec3>& corners, const glm::vec3& point, float arrivalRadius) {
    if (!corners.empty() && nearlyEqualVec3(corners.back(), point, arrivalRadius)) {
        return;
    }
    corners.push_back(point);
}

float pathLength(
    const glm::vec3& start,
    const std::vector<glm::vec3>& corners
) {
    float length = 0.0f;
    glm::vec3 previous = start;
    for (const glm::vec3& corner : corners) {
        length += glm::distance(previous, corner);
        previous = corner;
    }
    return length;
}

render::FrameCounterRecord makeNavigationCounter(const char* name, std::int64_t value) {
    return render::FrameCounterRecord{name, value, "Navigation"};
}

void applyPathResult(NavAgentComponent& agent, const glm::vec3& destination, std::vector<glm::vec3> corners) {
    agent.pathCorners = std::move(corners);
    agent.destination = destination;
    agent.moving = !agent.pathCorners.empty();
}

void snapAgentToResolvedStart(
    World& world,
    EntityId entity,
    TransformComponent& transform,
    const glm::vec3& resolvedStart
) {
    if (nearlyEqualVec3(
            transform.position,
            resolvedStart,
            kPolygonEpsilon)) {
        return;
    }
    transform.position = resolvedStart;
    world.markTransformsDirty(entity);
}

std::optional<NavigationSystem::PartialPathResult> consumePartialPathResult(
    const std::shared_ptr<NavigationSystem::PendingPathProgress>& progress
) {
    if (!progress) {
        return std::nullopt;
    }
    std::lock_guard lock(progress->mutex);
    if (!progress->partialPath.has_value()) {
        return std::nullopt;
    }
    std::optional<NavigationSystem::PartialPathResult> result = std::move(progress->partialPath);
    progress->partialPath.reset();
    return result;
}

ParentPathData buildStableIdPaths(const World& world) {
    ParentPathData data{};
    std::unordered_set<EntityId> entities{};
    for (EntityId entity : world.names.entities()) {
        entities.insert(entity);
    }
    for (EntityId entity : world.parents.entities()) {
        entities.insert(entity);
        const ParentComponent& parent = world.parents.get(entity);
        if (parent.parent.valid() && world.isAlive(parent.parent)) {
            entities.insert(parent.parent);
            data.childrenByParent[parent.parent].push_back(entity);
        }
    }
    for (EntityId entity : world.renderables.entities()) {
        entities.insert(entity);
    }
    for (EntityId entity : world.transforms.entities()) {
        entities.insert(entity);
    }

    std::vector<EntityId> roots{};
    roots.reserve(entities.size());
    for (EntityId entity : entities) {
        const ParentComponent* parent = world.parents.tryGet(entity);
        if (parent == nullptr || !parent->parent.valid() || !world.isAlive(parent->parent)) {
            roots.push_back(entity);
        }
    }

    const auto compareByLabel = [&world](EntityId lhs, EntityId rhs) {
        const std::string lhsLabel = entityName(world, lhs);
        const std::string rhsLabel = entityName(world, rhs);
        if (lhsLabel != rhsLabel) {
            return lhsLabel < rhsLabel;
        }
        return lhs.index < rhs.index;
    };

    std::sort(roots.begin(), roots.end(), compareByLabel);
    for (auto& [parent, children] : data.childrenByParent) {
        std::sort(children.begin(), children.end(), compareByLabel);
    }

    const auto visit = [&](const auto& self, EntityId entity, const std::string& parentPath) -> void {
        std::vector<EntityId> siblings{};
        const ParentComponent* parent = world.parents.tryGet(entity);
        if (parent != nullptr && parent->parent.valid() && world.isAlive(parent->parent)) {
            auto it = data.childrenByParent.find(parent->parent);
            if (it != data.childrenByParent.end()) {
                siblings = it->second;
            }
        } else {
            siblings = roots;
        }

        const std::string label = entityName(world, entity);
        int ordinal = 0;
        int matchingCount = 0;
        for (EntityId sibling : siblings) {
            if (entityName(world, sibling) != label) {
                continue;
            }
            if (sibling == entity) {
                ordinal = matchingCount;
            }
            ++matchingCount;
        }

        std::string segment = label;
        if (matchingCount > 1) {
            segment += "#" + std::to_string(ordinal);
        }
        const std::string path = parentPath.empty() ? segment : parentPath + "/" + segment;
        data.pathByEntity[entity] = path;

        auto it = data.childrenByParent.find(entity);
        if (it == data.childrenByParent.end()) {
            return;
        }
        for (EntityId child : it->second) {
            self(self, child, path);
        }
    };

    for (EntityId root : roots) {
        visit(visit, root, std::string{});
    }

    return data;
}

NavSourceTag defaultTagForLayer(render::RenderLayer layer) {
    switch (layer) {
        case render::RenderLayer::Ground:
            return NavSourceTag::Walkable;
        case render::RenderLayer::Actors:
            return NavSourceTag::Ignored;
        case render::RenderLayer::Geometry:
        default:
            return NavSourceTag::Blocking;
    }
}

bool pointInPolygonXZ(const glm::vec2& point, const std::vector<glm::vec2>& polygon) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1u; i < polygon.size(); j = i++) {
        const glm::vec2& a = polygon[i];
        const glm::vec2& b = polygon[j];
        const bool intersect = ((a.y > point.y) != (b.y > point.y)) &&
            (point.x < ((b.x - a.x) * (point.y - a.y) / ((b.y - a.y) + kPlaneEpsilon) + a.x));
        if (intersect) {
            inside = !inside;
        }
    }
    return inside;
}

bool pointOnSegmentXZ(const glm::vec2& point, const glm::vec2& a, const glm::vec2& b) {
    const glm::vec2 ab = b - a;
    const glm::vec2 ap = point - a;
    if (std::abs(cross2(ab, ap)) > kPolygonEpsilon) {
        return false;
    }
    return glm::dot(point - a, point - b) <= kPolygonEpsilon;
}

bool pointInOrOnPolygonXZ(const glm::vec2& point, const std::vector<glm::vec2>& polygon) {
    if (polygon.size() < 3u) {
        return false;
    }
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        if (pointOnSegmentXZ(point, polygon[index], polygon[(index + 1u) % polygon.size()])) {
            return true;
        }
    }
    return pointInPolygonXZ(point, polygon);
}

glm::vec2 closestPointOnSegmentXZ(
    const glm::vec2& point,
    const glm::vec2& segmentStart,
    const glm::vec2& segmentEnd
) {
    const glm::vec2 segment = segmentEnd - segmentStart;
    const float segmentLengthSquared = glm::dot(segment, segment);
    const float segmentT = segmentLengthSquared > kPlaneEpsilon
        ? std::clamp(
            glm::dot(point - segmentStart, segment) /
                segmentLengthSquared,
            0.0f,
            1.0f
        )
        : 0.0f;
    return segmentStart + segment * segmentT;
}

glm::vec2 closestPointOnPolygonXZ(
    const glm::vec2& point,
    const std::vector<glm::vec2>& polygon
) {
    if (polygon.empty() || pointInOrOnPolygonXZ(point, polygon)) {
        return point;
    }

    glm::vec2 closest = polygon.front();
    float closestDistanceSquared =
        glm::dot(point - closest, point - closest);
    for (std::size_t index = 0u; index < polygon.size(); ++index) {
        const glm::vec2& edgeStart = polygon[index];
        const glm::vec2& edgeEnd = polygon[(index + 1u) % polygon.size()];
        const glm::vec2 candidate =
            closestPointOnSegmentXZ(point, edgeStart, edgeEnd);
        const float distanceSquared =
            glm::dot(point - candidate, point - candidate);
        if (distanceSquared < closestDistanceSquared) {
            closest = candidate;
            closestDistanceSquared = distanceSquared;
        }
    }
    return closest;
}

bool pointInTriangleXZ(const glm::vec2& point, const WalkableTriangle& tri) {
    const float w0 = triArea2(point, tri.b, tri.c);
    const float w1 = triArea2(point, tri.c, tri.a);
    const float w2 = triArea2(point, tri.a, tri.b);
    const bool hasNegative = (w0 < -kPolygonEpsilon) || (w1 < -kPolygonEpsilon) || (w2 < -kPolygonEpsilon);
    const bool hasPositive = (w0 > kPolygonEpsilon) || (w1 > kPolygonEpsilon) || (w2 > kPolygonEpsilon);
    return !(hasNegative && hasPositive);
}

float polygonSignedArea(const std::vector<glm::vec2>& vertices) {
    float twiceArea = 0.0f;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const glm::vec2& a = vertices[index];
        const glm::vec2& b = vertices[(index + 1u) % vertices.size()];
        twiceArea += cross2(a, b);
    }
    return twiceArea * 0.5f;
}

bool polygonHasArea(const std::vector<glm::vec2>& vertices) {
    return vertices.size() >= 3u && std::abs(polygonSignedArea(vertices)) > kPolygonEpsilon;
}

void removeConsecutiveDuplicateVertices(std::vector<glm::vec2>& vertices) {
    std::vector<glm::vec2> filtered{};
    filtered.reserve(vertices.size());
    for (const glm::vec2& vertex : vertices) {
        if (!filtered.empty() && nearlyEqualVec2(filtered.back(), vertex)) {
            continue;
        }
        filtered.push_back(vertex);
    }
    if (filtered.size() >= 2u && nearlyEqualVec2(filtered.front(), filtered.back())) {
        filtered.pop_back();
    }
    vertices = std::move(filtered);
}

void removeColinearVertices(std::vector<glm::vec2>& vertices) {
    if (vertices.size() < 3u) {
        return;
    }

    bool changed = true;
    while (changed && vertices.size() >= 3u) {
        changed = false;
        for (std::size_t index = 0; index < vertices.size(); ++index) {
            const std::size_t prevIndex = (index + vertices.size() - 1u) % vertices.size();
            const std::size_t nextIndex = (index + 1u) % vertices.size();
            const glm::vec2& prev = vertices[prevIndex];
            const glm::vec2& current = vertices[index];
            const glm::vec2& next = vertices[nextIndex];
            if (pointOnSegmentXZ(current, prev, next)) {
                vertices.erase(vertices.begin() + static_cast<std::ptrdiff_t>(index));
                changed = true;
                break;
            }
        }
    }
}

std::vector<glm::vec2> normalizePolygonVertices(const std::vector<glm::vec2>& vertices) {
    std::vector<glm::vec2> normalized = vertices;
    removeConsecutiveDuplicateVertices(normalized);
    removeColinearVertices(normalized);
    if (normalized.size() >= 3u && polygonSignedArea(normalized) < 0.0f) {
        std::reverse(normalized.begin(), normalized.end());
    }
    return normalized;
}

glm::vec2 polygonCentroidXZ(const std::vector<glm::vec2>& vertices) {
    glm::vec2 centroid(0.0f);
    if (vertices.empty()) {
        return centroid;
    }

    float twiceArea = 0.0f;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const glm::vec2& a = vertices[index];
        const glm::vec2& b = vertices[(index + 1u) % vertices.size()];
        const float cross = cross2(a, b);
        twiceArea += cross;
        centroid += (a + b) * cross;
    }
    if (std::abs(twiceArea) <= kPolygonEpsilon) {
        for (const glm::vec2& vertex : vertices) {
            centroid += vertex;
        }
        return centroid / static_cast<float>(vertices.size());
    }
    return centroid / (3.0f * twiceArea);
}

bool polygonValid(const NavPolygon& polygon) {
    return polygon.id >= 0 && polygon.verticesXZ.size() >= 3u;
}

int orientationSign(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    const float area = triArea2(a, b, c);
    if (area > kPolygonEpsilon) {
        return 1;
    }
    if (area < -kPolygonEpsilon) {
        return -1;
    }
    return 0;
}

bool segmentsIntersectInclusive(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec2& d) {
    const int o1 = orientationSign(a, b, c);
    const int o2 = orientationSign(a, b, d);
    const int o3 = orientationSign(c, d, a);
    const int o4 = orientationSign(c, d, b);

    if (o1 != o2 && o3 != o4) {
        return true;
    }
    if (o1 == 0 && pointOnSegmentXZ(c, a, b)) {
        return true;
    }
    if (o2 == 0 && pointOnSegmentXZ(d, a, b)) {
        return true;
    }
    if (o3 == 0 && pointOnSegmentXZ(a, c, d)) {
        return true;
    }
    if (o4 == 0 && pointOnSegmentXZ(b, c, d)) {
        return true;
    }
    return false;
}

bool polygonIsSimple(const std::vector<glm::vec2>& vertices) {
    if (vertices.size() < 3u) {
        return false;
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const glm::vec2& a = vertices[i];
        const glm::vec2& b = vertices[(i + 1u) % vertices.size()];
        if (nearlyEqualVec2(a, b)) {
            return false;
        }
        for (std::size_t j = i + 1u; j < vertices.size(); ++j) {
            const std::size_t iNext = (i + 1u) % vertices.size();
            const std::size_t jNext = (j + 1u) % vertices.size();
            if (i == j || i == jNext || iNext == j) {
                continue;
            }
            if (segmentsIntersectInclusive(a, b, vertices[j], vertices[jNext])) {
                return false;
            }
        }
    }
    return true;
}

std::vector<std::array<glm::vec2, 3u>> triangulateSimplePolygon(const std::vector<glm::vec2>& polygon) {
    std::vector<std::array<glm::vec2, 3u>> triangles{};
    if (polygon.size() < 3u) {
        return triangles;
    }
    if (polygon.size() == 3u) {
        triangles.push_back({polygon[0], polygon[1], polygon[2]});
        return triangles;
    }

    std::vector<std::size_t> remaining{};
    remaining.reserve(polygon.size());
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        remaining.push_back(index);
    }

    std::size_t guard = 0u;
    while (remaining.size() > 3u && guard < polygon.size() * polygon.size()) {
        bool clippedEar = false;
        for (std::size_t localIndex = 0; localIndex < remaining.size(); ++localIndex) {
            const std::size_t prevLocal = (localIndex + remaining.size() - 1u) % remaining.size();
            const std::size_t nextLocal = (localIndex + 1u) % remaining.size();
            const glm::vec2& prev = polygon[remaining[prevLocal]];
            const glm::vec2& current = polygon[remaining[localIndex]];
            const glm::vec2& next = polygon[remaining[nextLocal]];
            if (triArea2(prev, current, next) <= kPolygonEpsilon) {
                continue;
            }

            WalkableTriangle ear{};
            ear.a = prev;
            ear.b = current;
            ear.c = next;

            bool containsOtherVertex = false;
            for (std::size_t candidateIndex = 0; candidateIndex < remaining.size(); ++candidateIndex) {
                if (candidateIndex == prevLocal || candidateIndex == localIndex || candidateIndex == nextLocal) {
                    continue;
                }
                if (pointInTriangleXZ(polygon[remaining[candidateIndex]], ear)) {
                    containsOtherVertex = true;
                    break;
                }
            }
            if (containsOtherVertex) {
                continue;
            }

            triangles.push_back({prev, current, next});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(localIndex));
            clippedEar = true;
            break;
        }
        if (!clippedEar) {
            triangles.clear();
            return triangles;
        }
        ++guard;
    }

    if (remaining.size() == 3u) {
        triangles.push_back({
            polygon[remaining[0]],
            polygon[remaining[1]],
            polygon[remaining[2]]
        });
    }
    return triangles;
}

bool blockerOverlapsLayer(const BlockingFootprint& blocker, float elevationY) {
    return elevationY >= blocker.minY - kLayerGroupingEpsilon &&
        elevationY <= blocker.maxY + kLayerGroupingEpsilon;
}

std::optional<BlockingFootprint> makeBlockingFootprint(
    std::vector<glm::vec2> vertices,
    float minY,
    float maxY
) {
    vertices = normalizePolygonVertices(vertices);
    if (!polygonHasArea(vertices)) {
        return std::nullopt;
    }

    BlockingFootprint footprint{};
    footprint.minY = std::min(minY, maxY);
    footprint.maxY = std::max(minY, maxY);
    footprint.verticesXZ = std::move(vertices);
    footprint.minXZ = footprint.verticesXZ.front();
    footprint.maxXZ = footprint.verticesXZ.front();
    for (const glm::vec2& vertex : footprint.verticesXZ) {
        footprint.minXZ = glm::min(footprint.minXZ, vertex);
        footprint.maxXZ = glm::max(footprint.maxXZ, vertex);
    }
    return footprint;
}

bool boundsOverlapXZ(
    const glm::vec2& lhsMin,
    const glm::vec2& lhsMax,
    const glm::vec2& rhsMin,
    const glm::vec2& rhsMax
) {
    return lhsMax.x >= rhsMin.x - kPolygonEpsilon &&
        rhsMax.x >= lhsMin.x - kPolygonEpsilon &&
        lhsMax.y >= rhsMin.y - kPolygonEpsilon &&
        rhsMax.y >= lhsMin.y - kPolygonEpsilon;
}

std::pair<glm::vec2, glm::vec2> polygonBoundsXZ(const std::vector<glm::vec2>& polygon) {
    glm::vec2 minPoint = polygon.front();
    glm::vec2 maxPoint = polygon.front();
    for (const glm::vec2& vertex : polygon) {
        minPoint = glm::min(minPoint, vertex);
        maxPoint = glm::max(maxPoint, vertex);
    }
    return {minPoint, maxPoint};
}

std::vector<std::vector<glm::vec2>> subtractConvexPolygon(
    const std::vector<glm::vec2>& subject,
    const std::vector<glm::vec2>& clipperVertices,
    const glm::vec2& clipperMin,
    const glm::vec2& clipperMax
) {
    if (!polygonHasArea(subject)) {
        return {};
    }
    const auto [subjectMin, subjectMax] = polygonBoundsXZ(subject);
    if (!boundsOverlapXZ(
            subjectMin,
            subjectMax,
            clipperMin,
            clipperMax)) {
        return {subject};
    }

    std::vector<std::vector<glm::vec2>> outsidePieces{};
    std::vector<glm::vec2> intersection = subject;
    // Each outside piece satisfies all prior clipper half-planes, so the
    // pieces are convex and non-overlapping while their union is subject - clipper.
    for (std::size_t edgeIndex = 0; edgeIndex < clipperVertices.size(); ++edgeIndex) {
        const glm::vec2& edgeA = clipperVertices[edgeIndex];
        const glm::vec2& edgeB = clipperVertices[
            (edgeIndex + 1u) % clipperVertices.size()
        ];

        std::vector<glm::vec2> outside =
            clipConvexPolygonAgainstHalfPlane(
                intersection,
                edgeA,
                edgeB,
                false,
                0.0f
            );
        if (polygonHasArea(outside)) {
            outsidePieces.push_back(std::move(outside));
        }
        intersection = clipConvexPolygonAgainstHalfPlane(
            intersection,
            edgeA,
            edgeB,
            true,
            0.0f
        );
        if (!polygonHasArea(intersection)) {
            break;
        }
    }
    return outsidePieces;
}

std::vector<std::vector<glm::vec2>> subtractConvexPolygon(
    const std::vector<glm::vec2>& subject,
    const BlockingFootprint& clipper
) {
    return subtractConvexPolygon(
        subject,
        clipper.verticesXZ,
        clipper.minXZ,
        clipper.maxXZ
    );
}

bool convexPolygonsHaveInteriorOverlap(
    const std::vector<glm::vec2>& lhs,
    const std::vector<glm::vec2>& rhs
) {
    if (!polygonHasArea(lhs) || !polygonHasArea(rhs)) {
        return false;
    }
    const auto removedArea = [](
        const std::vector<glm::vec2>& subject,
        const std::vector<glm::vec2>& clipper
    ) {
        const auto [clipperMin, clipperMax] = polygonBoundsXZ(clipper);
        const double subjectArea = std::abs(
            static_cast<double>(polygonSignedArea(subject))
        );
        double outsideArea = 0.0;
        for (const std::vector<glm::vec2>& piece : subtractConvexPolygon(
                 subject,
                 clipper,
                 clipperMin,
                 clipperMax)) {
            outsideArea += std::abs(
                static_cast<double>(polygonSignedArea(piece))
            );
        }
        return std::max(0.0, subjectArea - outsideArea);
    };
    const auto perimeter = [](const std::vector<glm::vec2>& polygon) {
        double result = 0.0;
        for (std::size_t index = 0u; index < polygon.size(); ++index) {
            result += glm::distance(
                polygon[index],
                polygon[(index + 1u) % polygon.size()]
            );
        }
        return result;
    };
    const double areaTolerance = static_cast<double>(kPolygonEpsilon) *
        std::max({1.0, perimeter(lhs), perimeter(rhs)}) * 2.0;
    return std::max(removedArea(lhs, rhs), removedArea(rhs, lhs)) >
        areaTolerance;
}

bool convexCellSetHasInteriorOverlap(
    const std::vector<NavRuntimeCell>& cells
) {
    for (std::size_t lhs = 0u; lhs < cells.size(); ++lhs) {
        const auto [lhsMin, lhsMax] =
            polygonBoundsXZ(cells[lhs].verticesXZ);
        for (std::size_t rhs = lhs + 1u; rhs < cells.size(); ++rhs) {
            if (std::abs(cells[lhs].elevationY - cells[rhs].elevationY) >
                    kLayerGroupingEpsilon) {
                continue;
            }
            const auto [rhsMin, rhsMax] =
                polygonBoundsXZ(cells[rhs].verticesXZ);
            if (boundsOverlapXZ(lhsMin, lhsMax, rhsMin, rhsMax) &&
                convexPolygonsHaveInteriorOverlap(
                    cells[lhs].verticesXZ,
                    cells[rhs].verticesXZ)) {
                return true;
            }
        }
    }
    return false;
}

glm::vec3 transformPoint3(const glm::mat4& matrix, const glm::vec3& point) {
    return glm::vec3(matrix * glm::vec4(point, 1.0f));
}

EntityId objectRootEntity(const World& world, EntityId entity) {
    EntityId root = entity;
    while (const ParentComponent* parent = world.parents.tryGet(root)) {
        if (!parent->parent.valid() || !world.isAlive(parent->parent)) {
            break;
        }
        root = parent->parent;
    }
    return root;
}

const glm::mat4* cachedWorldMatrix(const World& world, EntityId entity) {
    if (entity.index >= world.transformCache_.size() || !world.transformCache_[entity.index].valid) {
        return nullptr;
    }
    return &world.transformCache_[entity.index].worldMatrix;
}

void appendBoxColliderFootprint(
    const BoxColliderComponent& collider,
    const glm::mat4& worldMatrix,
    std::vector<BlockingFootprint>& footprints
) {
    const OrientedBox box = makeOrientedBox(worldMatrix, collider);
    std::vector<glm::vec2> projectedCorners{};
    projectedCorners.reserve(box.corners.size());
    float minY = box.corners.front().y;
    float maxY = box.corners.front().y;
    for (const glm::vec3& corner : box.corners) {
        projectedCorners.emplace_back(corner.x, corner.z);
        minY = std::min(minY, corner.y);
        maxY = std::max(maxY, corner.y);
    }
    if (auto footprint = makeBlockingFootprint(buildConvexHull(std::move(projectedCorners)), minY, maxY)) {
        footprints.push_back(std::move(*footprint));
    }
}

void appendSphereColliderFootprint(
    const SphereColliderComponent& collider,
    const glm::mat4& worldMatrix,
    std::vector<BlockingFootprint>& footprints
) {
    constexpr int kSphereFootprintSides = 16;
    const glm::vec3 center = transformPoint3(worldMatrix, collider.center);
    const float maxScale = std::max({
        glm::length(glm::vec3(worldMatrix[0])),
        glm::length(glm::vec3(worldMatrix[1])),
        glm::length(glm::vec3(worldMatrix[2]))
    });
    const float radius = std::max(collider.radius * maxScale, kPolygonEpsilon);
    const float circumradius = radius / std::cos(3.141592653589793f / static_cast<float>(kSphereFootprintSides));

    std::vector<glm::vec2> vertices{};
    vertices.reserve(kSphereFootprintSides);
    for (int index = 0; index < kSphereFootprintSides; ++index) {
        const float angle = kTau * static_cast<float>(index) / static_cast<float>(kSphereFootprintSides);
        vertices.emplace_back(
            center.x + std::cos(angle) * circumradius,
            center.z + std::sin(angle) * circumradius
        );
    }
    if (auto footprint = makeBlockingFootprint(std::move(vertices), center.y - radius, center.y + radius)) {
        footprints.push_back(std::move(*footprint));
    }
}

std::vector<NavRuntimeCell> buildBoundedDefaultHitboxParts(std::vector<NavRuntimeCell> cells) {
    if (cells.size() <= kMaxProjectedCellsPerDefaultHitboxPart) {
        return cells;
    }

    std::vector<NavRuntimeCell> parts{};
    const auto partition = [&](const auto& self, std::vector<NavRuntimeCell> cluster) -> void {
        if (cluster.size() <= kMaxProjectedCellsPerDefaultHitboxPart) {
            std::vector<glm::vec2> points{};
            for (const NavRuntimeCell& cell : cluster) {
                points.insert(points.end(), cell.verticesXZ.begin(), cell.verticesXZ.end());
            }
            std::vector<glm::vec2> hull = buildConvexHull(std::move(points));
            if (polygonHasArea(hull)) {
                parts.push_back(NavRuntimeCell{0.0f, std::move(hull)});
            }
            return;
        }

        glm::vec2 minCenter(std::numeric_limits<float>::max());
        glm::vec2 maxCenter(std::numeric_limits<float>::lowest());
        for (const NavRuntimeCell& cell : cluster) {
            const glm::vec2 center = polygonCentroidXZ(cell.verticesXZ);
            minCenter = glm::min(minCenter, center);
            maxCenter = glm::max(maxCenter, center);
        }
        const bool splitX = maxCenter.x - minCenter.x >= maxCenter.y - minCenter.y;
        std::sort(cluster.begin(), cluster.end(), [splitX](const NavRuntimeCell& lhs, const NavRuntimeCell& rhs) {
            const glm::vec2 lhsCenter = polygonCentroidXZ(lhs.verticesXZ);
            const glm::vec2 rhsCenter = polygonCentroidXZ(rhs.verticesXZ);
            const float lhsPrimary = splitX ? lhsCenter.x : lhsCenter.y;
            const float rhsPrimary = splitX ? rhsCenter.x : rhsCenter.y;
            if (lhsPrimary != rhsPrimary) {
                return lhsPrimary < rhsPrimary;
            }
            return (splitX ? lhsCenter.y : lhsCenter.x) < (splitX ? rhsCenter.y : rhsCenter.x);
        });

        const std::size_t middle = cluster.size() / 2u;
        std::vector<NavRuntimeCell> rhs(
            std::make_move_iterator(cluster.begin() + static_cast<std::ptrdiff_t>(middle)),
            std::make_move_iterator(cluster.end())
        );
        cluster.erase(cluster.begin() + static_cast<std::ptrdiff_t>(middle), cluster.end());
        self(self, std::move(cluster));
        self(self, std::move(rhs));
    };

    partition(partition, std::move(cells));
    return parts;
}

void appendDefaultGeometryFootprints(
    const render::Mesh& mesh,
    const glm::mat4& worldMatrix,
    std::vector<BlockingFootprint>& footprints
) {
    if (mesh.positions.empty()) {
        return;
    }

    std::vector<glm::vec3> worldPositions{};
    worldPositions.reserve(mesh.positions.size());
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    for (const glm::vec3& position : mesh.positions) {
        const glm::vec3 worldPosition = transformPoint3(worldMatrix, position);
        worldPositions.push_back(worldPosition);
        minY = std::min(minY, worldPosition.y);
        maxY = std::max(maxY, worldPosition.y);
    }

    std::unordered_set<std::string> emitted{};
    std::vector<NavRuntimeCell> projectedCells{};
    if (!mesh.indices.empty() && mesh.indices.size() % 3u == 0u) {
        for (std::size_t index = 0; index < mesh.indices.size(); index += 3u) {
            const unsigned int ia = mesh.indices[index];
            const unsigned int ib = mesh.indices[index + 1u];
            const unsigned int ic = mesh.indices[index + 2u];
            if (ia >= worldPositions.size() || ib >= worldPositions.size() || ic >= worldPositions.size()) {
                continue;
            }
            std::vector<glm::vec2> triangle{
                glm::vec2(worldPositions[ia].x, worldPositions[ia].z),
                glm::vec2(worldPositions[ib].x, worldPositions[ib].z),
                glm::vec2(worldPositions[ic].x, worldPositions[ic].z),
            };
            const std::string key = canonicalPolygonKey(triangle);
            if (key.empty() || !emitted.insert(key).second) {
                continue;
            }
            triangle = normalizePolygonVertices(triangle);
            if (polygonHasArea(triangle)) {
                projectedCells.push_back(NavRuntimeCell{0.0f, std::move(triangle)});
            }
        }
        if (!emitted.empty()) {
            mergeAdjacentConvexCells(projectedCells);
            projectedCells = buildBoundedDefaultHitboxParts(std::move(projectedCells));
            for (NavRuntimeCell& cell : projectedCells) {
                if (auto footprint = makeBlockingFootprint(std::move(cell.verticesXZ), minY, maxY)) {
                    footprints.push_back(std::move(*footprint));
                }
            }
            return;
        }
    }

    std::vector<glm::vec2> projected{};
    projected.reserve(worldPositions.size());
    for (const glm::vec3& position : worldPositions) {
        projected.emplace_back(position.x, position.z);
    }
    if (auto footprint = makeBlockingFootprint(buildConvexHull(std::move(projected)), minY, maxY)) {
        footprints.push_back(std::move(*footprint));
    }
}

std::vector<BlockingFootprint> buildBlockingFootprints(const World& world) {
    std::unordered_map<EntityId, std::vector<EntityId>> blockingSourcesByRoot{};
    for (EntityId entity : world.navSources.entities()) {
        if (world.navSources.get(entity).effectiveTag == NavSourceTag::Blocking) {
            blockingSourcesByRoot[objectRootEntity(world, entity)].push_back(entity);
        }
    }

    std::unordered_map<EntityId, std::vector<BlockingFootprint>> explicitFootprintsByRoot{};
    for (EntityId entity : world.boxColliders.entities()) {
        const BoxColliderComponent& collider = world.boxColliders.get(entity);
        const EntityId root = objectRootEntity(world, entity);
        const glm::mat4* worldMatrix = cachedWorldMatrix(world, entity);
        if (collider.isTrigger || worldMatrix == nullptr || !blockingSourcesByRoot.contains(root)) {
            continue;
        }
        appendBoxColliderFootprint(collider, *worldMatrix, explicitFootprintsByRoot[root]);
    }
    for (EntityId entity : world.sphereColliders.entities()) {
        const SphereColliderComponent& collider = world.sphereColliders.get(entity);
        const EntityId root = objectRootEntity(world, entity);
        const glm::mat4* worldMatrix = cachedWorldMatrix(world, entity);
        if (collider.isTrigger || worldMatrix == nullptr || !blockingSourcesByRoot.contains(root)) {
            continue;
        }
        appendSphereColliderFootprint(collider, *worldMatrix, explicitFootprintsByRoot[root]);
    }

    std::vector<BlockingFootprint> footprints{};
    for (const auto& [root, sources] : blockingSourcesByRoot) {
        auto explicitIt = explicitFootprintsByRoot.find(root);
        if (explicitIt != explicitFootprintsByRoot.end() && !explicitIt->second.empty()) {
            footprints.insert(
                footprints.end(),
                std::make_move_iterator(explicitIt->second.begin()),
                std::make_move_iterator(explicitIt->second.end())
            );
            continue;
        }

        for (EntityId sourceEntity : sources) {
            const NavSourceGeometryComponent* geometry = world.navSourceGeometry.tryGet(sourceEntity);
            const glm::mat4* worldMatrix = cachedWorldMatrix(world, sourceEntity);
            if (geometry == nullptr || !geometry->mesh || worldMatrix == nullptr) {
                continue;
            }
            appendDefaultGeometryFootprints(*geometry->mesh, *worldMatrix, footprints);
        }
    }
    return footprints;
}

void appendConvexPolygonToUnion(
    std::vector<NavRuntimeCell>& cells,
    std::vector<glm::vec2> polygon,
    float elevationY
) {
    polygon = normalizePolygonVertices(polygon);
    if (!polygonHasArea(polygon)) {
        return;
    }

    std::vector<std::vector<glm::vec2>> additions{std::move(polygon)};
    for (const NavRuntimeCell& existing : cells) {
        const auto existingFootprint = makeBlockingFootprint(existing.verticesXZ, elevationY, elevationY);
        if (!existingFootprint.has_value()) {
            continue;
        }
        std::vector<std::vector<glm::vec2>> next{};
        for (const std::vector<glm::vec2>& addition : additions) {
            std::vector<std::vector<glm::vec2>> pieces = subtractConvexPolygon(addition, *existingFootprint);
            next.insert(next.end(), std::make_move_iterator(pieces.begin()), std::make_move_iterator(pieces.end()));
        }
        additions = std::move(next);
        if (additions.empty()) {
            return;
        }
    }

    for (std::vector<glm::vec2>& addition : additions) {
        cells.push_back(NavRuntimeCell{elevationY, std::move(addition)});
    }
}

std::vector<NavPolygon> buildPolygonsForLayer(const LayerBuildData& layer, int& nextPolygonId) {
    std::vector<NavRuntimeCell> cells{};
    for (const WalkableTriangle& triangle : layer.triangles) {
        appendConvexPolygonToUnion(cells, {triangle.a, triangle.b, triangle.c}, layer.elevationY);
    }

    mergeAdjacentConvexCells(cells);
    std::vector<NavRuntimeCell> blockerCells{};
    for (const BlockingFootprint& blocker : layer.blockers) {
        if (!blockerOverlapsLayer(blocker, layer.elevationY)) {
            continue;
        }
        appendConvexPolygonToUnion(blockerCells, blocker.verticesXZ, layer.elevationY);
    }
    mergeAdjacentConvexCells(blockerCells);

    for (const NavRuntimeCell& blockerCell : blockerCells) {
        const auto blocker = makeBlockingFootprint(
            blockerCell.verticesXZ,
            layer.elevationY,
            layer.elevationY
        );
        if (!blocker.has_value()) {
            continue;
        }
        std::vector<NavRuntimeCell> next{};
        for (const NavRuntimeCell& cell : cells) {
            std::vector<std::vector<glm::vec2>> pieces = subtractConvexPolygon(cell.verticesXZ, *blocker);
            for (std::vector<glm::vec2>& piece : pieces) {
                next.push_back(NavRuntimeCell{layer.elevationY, std::move(piece)});
            }
        }
        cells = std::move(next);
        if (cells.empty()) {
            break;
        }
    }
    mergeAdjacentConvexCells(cells);

    std::vector<NavPolygon> polygons{};
    polygons.reserve(cells.size());
    for (NavRuntimeCell& cell : cells) {
        polygons.push_back(NavPolygon{nextPolygonId++, layer.elevationY, std::move(cell.verticesXZ)});
    }
    return polygons;
}

void addTriangleToLayer(
    std::vector<LayerBuildData>& layers,
    float elevationY,
    const WalkableTriangle& triangle
) {
    for (LayerBuildData& layer : layers) {
        if (std::abs(layer.elevationY - elevationY) <= kLayerGroupingEpsilon) {
            layer.triangles.push_back(triangle);
            return;
        }
    }
    layers.push_back(LayerBuildData{elevationY, {triangle}, {}});
}


void ensureCCW(std::vector<glm::vec2>& vertices) {
    if (vertices.size() < 3u) {
        return;
    }
    float twiceArea = 0.0f;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const std::size_t j = (i + 1u) % vertices.size();
        twiceArea += cross2(vertices[i], vertices[j]);
    }
    if (twiceArea < 0.0f) {
        std::reverse(vertices.begin(), vertices.end());
    }
}

std::vector<glm::vec2> clipConvexPolygons(
    const std::vector<glm::vec2>& subject,
    const std::vector<glm::vec2>& clip
) {
    if (subject.size() < 3u || clip.size() < 3u) {
        return {};
    }
    std::vector<glm::vec2> clipCCW = clip;
    ensureCCW(clipCCW);

    std::vector<glm::vec2> output = subject;
    for (std::size_t i = 0; i < clipCCW.size() && output.size() >= 3u; ++i) {
        const std::vector<glm::vec2> input = output;
        output.clear();
        const glm::vec2& edgeStart = clipCCW[i];
        const glm::vec2& edgeEnd = clipCCW[(i + 1u) % clipCCW.size()];
        const glm::vec2 edgeDir = edgeEnd - edgeStart;

        for (std::size_t j = 0; j < input.size(); ++j) {
            const glm::vec2& current = input[j];
            const glm::vec2& previous = input[(j + input.size() - 1u) % input.size()];
            const float currentSide = cross2(edgeDir, current - edgeStart);
            const float previousSide = cross2(edgeDir, previous - edgeStart);
            const bool currentInside = currentSide >= -kPolygonEpsilon;
            const bool previousInside = previousSide >= -kPolygonEpsilon;

            if (currentInside) {
                if (!previousInside) {
                    const glm::vec2 d = current - previous;
                    const float denom = cross2(edgeDir, d);
                    if (std::abs(denom) > kPlaneEpsilon) {
                        const float t = std::clamp(
                            cross2(edgeDir, edgeStart - previous) / denom, 0.0f, 1.0f);
                        output.push_back(previous + t * d);
                    }
                }
                output.push_back(current);
            } else if (previousInside) {
                const glm::vec2 d = current - previous;
                const float denom = cross2(edgeDir, d);
                if (std::abs(denom) > kPlaneEpsilon) {
                    const float t = std::clamp(
                        cross2(edgeDir, edgeStart - previous) / denom, 0.0f, 1.0f);
                    output.push_back(previous + t * d);
                }
            }
        }
    }
    return output;
}

std::vector<glm::vec2> clipConvexPolygonAgainstHalfPlane(
    const std::vector<glm::vec2>& polygon,
    const glm::vec2& lineA,
    const glm::vec2& lineB,
    bool keepLeft,
    float tolerance
) {
    if (polygon.size() < 3u) {
        return {};
    }

    const glm::vec2 lineDir = lineB - lineA;
    if (glm::length(lineDir) <= kPolygonEpsilon) {
        return polygon;
    }

    auto signedSide = [&](const glm::vec2& point) {
        const float side = cross2(lineDir, point - lineA);
        return keepLeft ? side : -side;
    };

    std::vector<glm::vec2> output{};
    output.reserve(polygon.size() + 1u);
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const glm::vec2& current = polygon[index];
        const glm::vec2& previous = polygon[(index + polygon.size() - 1u) % polygon.size()];
        const float currentSide = signedSide(current);
        const float previousSide = signedSide(previous);
        const bool currentInside = currentSide >= -tolerance;
        const bool previousInside = previousSide >= -tolerance;

        if (currentInside != previousInside) {
            const glm::vec2 segmentDir = current - previous;
            const float denominator = cross2(lineDir, segmentDir);
            if (std::abs(denominator) > kPlaneEpsilon) {
                const float t = std::clamp(cross2(lineDir, lineA - previous) / denominator, 0.0f, 1.0f);
                output.push_back(previous + t * segmentDir);
            }
        }
        if (currentInside) {
            output.push_back(current);
        }
    }

    return normalizePolygonVertices(output);
}

bool convexFootprintInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const std::vector<glm::vec2>& footprint,
    float elevationY,
    const std::vector<std::size_t>* candidateCells
) {
    const float sweptArea = std::abs(polygonSignedArea(footprint));
    if (sweptArea <= kPolygonEpsilon) {
        return true;
    }

    glm::vec2 boundsMin(std::numeric_limits<float>::max());
    glm::vec2 boundsMax(std::numeric_limits<float>::lowest());
    for (const glm::vec2& point : footprint) {
        boundsMin = glm::min(boundsMin, point);
        boundsMax = glm::max(boundsMax, point);
    }
    // Subtract the walkable cells from the footprint instead of adding clipped
    // areas. The latter double-counts overlaps in imported/authored meshes and
    // can accept a footprint that actually crosses a hole.
    std::vector<std::vector<glm::vec2>> uncovered{footprint};
    const double coverageTolerance = std::max(
        static_cast<double>(kPolygonEpsilon) * 0.25,
        static_cast<double>(sweptArea) * 1.0e-6
    );
    const auto cellCouldOverlapFootprint = [&](std::size_t cellIndex) {
        if (cellIndex >= runtime.bakedCells.size()) {
            return false;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        return std::abs(elevationY - cell.elevationY) <=
                kLayerGroupingEpsilon &&
            (cellIndex >= runtime.bakedCellMinXZ.size() ||
             cellIndex >= runtime.bakedCellMaxXZ.size() ||
             boundsOverlapXZ(
                 boundsMin,
                 boundsMax,
                 runtime.bakedCellMinXZ[cellIndex],
                 runtime.bakedCellMaxXZ[cellIndex]));
    };
    if (!runtime.bakedCellsHaveInteriorOverlap) {
        double coveredArea = 0.0;
        const auto accumulateCell = [&](std::size_t cellIndex) {
            if (!cellCouldOverlapFootprint(cellIndex)) {
                return;
            }
            const std::vector<glm::vec2> clipped = clipConvexPolygons(
                footprint,
                runtime.bakedCells[cellIndex].verticesXZ
            );
            if (clipped.size() >= 3u) {
                coveredArea += std::abs(
                    static_cast<double>(polygonSignedArea(clipped))
                );
            }
        };
        if (candidateCells != nullptr) {
            for (std::size_t cellIndex : *candidateCells) {
                accumulateCell(cellIndex);
            }
        } else {
            for (std::size_t cellIndex = 0u;
                 cellIndex < runtime.bakedCells.size();
                 ++cellIndex) {
                accumulateCell(cellIndex);
            }
        }
        return coveredArea + coverageTolerance >=
            static_cast<double>(sweptArea);
    }
    const auto subtractCell = [&](std::size_t cellIndex) {
        if (uncovered.empty() || !cellCouldOverlapFootprint(cellIndex)) {
            return;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        const glm::vec2 cellMin = cellIndex < runtime.bakedCellMinXZ.size()
            ? runtime.bakedCellMinXZ[cellIndex]
            : polygonBoundsXZ(cell.verticesXZ).first;
        const glm::vec2 cellMax = cellIndex < runtime.bakedCellMaxXZ.size()
            ? runtime.bakedCellMaxXZ[cellIndex]
            : polygonBoundsXZ(cell.verticesXZ).second;
        std::vector<std::vector<glm::vec2>> remaining{};
        for (const std::vector<glm::vec2>& piece : uncovered) {
            std::vector<std::vector<glm::vec2>> pieces =
                subtractConvexPolygon(
                    piece,
                    cell.verticesXZ,
                    cellMin,
                    cellMax
                );
            for (std::vector<glm::vec2>& outside : pieces) {
                remaining.push_back(std::move(outside));
            }
        }
        uncovered = std::move(remaining);
    };
    if (candidateCells != nullptr) {
        for (std::size_t cellIndex : *candidateCells) {
            subtractCell(cellIndex);
            if (uncovered.empty()) {
                break;
            }
        }
    } else {
        for (std::size_t cellIndex = 0u;
             cellIndex < runtime.bakedCells.size() && !uncovered.empty();
             ++cellIndex) {
            subtractCell(cellIndex);
        }
    }
    double uncoveredArea = 0.0;
    for (const std::vector<glm::vec2>& piece : uncovered) {
        uncoveredArea += std::abs(
            static_cast<double>(polygonSignedArea(piece))
        );
        if (uncoveredArea > coverageTolerance) {
            return false;
        }
    }
    return true;
}

bool boxSweepInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection,
    const std::vector<std::size_t>* candidateCells
) {
    if (profile.shape != AgentClearanceShape::Box ||
        std::abs(from.y - to.y) > kLayerGroupingEpsilon) {
        return false;
    }

    const glm::vec2 forward = travelDirectionForSegment(
        from,
        to,
        preferredTravelDirection
    );
    const glm::vec2 right(forward.y, -forward.x);
    const glm::vec2 centerOffset =
        rotateLocalXZToPlanar(profile.centerXZ, forward);
    const glm::vec2 startCenter(from.x, from.z);
    const glm::vec2 endCenter(to.x, to.z);
    const glm::vec2 lateral = right * profile.boxHalfExtentsXZ.x;
    const glm::vec2 longitudinal = forward * profile.boxHalfExtentsXZ.y;
    const glm::vec2 back = startCenter + centerOffset - longitudinal;
    const glm::vec2 front = endCenter + centerOffset + longitudinal;
    const std::vector<glm::vec2> swept{
        back + lateral,
        front + lateral,
        front - lateral,
        back - lateral,
    };
    for (std::size_t edgeIndex = 0u;
         edgeIndex < swept.size();
         ++edgeIndex) {
        const glm::vec2& edgeStart = swept[edgeIndex];
        const glm::vec2& edgeEnd =
            swept[(edgeIndex + 1u) % swept.size()];
        if (!segmentInsideBakedWalkableSurface(
                runtime,
                glm::vec3(edgeStart.x, from.y, edgeStart.y),
                glm::vec3(edgeEnd.x, from.y, edgeEnd.y),
                candidateCells)) {
            return false;
        }
    }
    return convexFootprintInsideWalkableSurface(
        runtime,
        swept,
        from.y,
        candidateCells
    );
}

bool sphereSweepInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection,
    const std::vector<std::size_t>* candidateCells
) {
    if (profile.shape != AgentClearanceShape::Sphere ||
        std::abs(from.y - to.y) > kLayerGroupingEpsilon) {
        return false;
    }

    constexpr int kCapsuleSides = 32;
    const glm::vec2 forward = travelDirectionForSegment(
        from,
        to,
        preferredTravelDirection
    );
    const glm::vec2 centerOffset =
        rotateLocalXZToPlanar(profile.centerXZ, forward);
    const glm::vec2 startCenter(from.x, from.z);
    const glm::vec2 endCenter(to.x, to.z);
    const float radius = std::max(0.0f, profile.sphereRadius);
    if (radius <= kPolygonEpsilon) {
        return true;
    }

    // A regular polygon whose inradius is the collider radius contains the
    // exact disk. Its Minkowski sum with the travelled segment is therefore a
    // conservative capsule: area coverage cannot miss a hole between samples.
    const float circumradius = radius / std::cos(
        3.14159265358979323846f / static_cast<float>(kCapsuleSides)
    );
    std::vector<glm::vec2> capsulePoints{};
    capsulePoints.reserve(static_cast<std::size_t>(kCapsuleSides) * 2u);
    for (int index = 0; index < kCapsuleSides; ++index) {
        // Half-step rotation makes the polygon sides (rather than protruding
        // vertices) tangent on the world axes. The polygon still contains the
        // exact disk, while an exact circle tangent to an axis-aligned navmesh
        // boundary remains a valid placement.
        const float angle = kTau *
            (static_cast<float>(index) + 0.5f) /
            static_cast<float>(kCapsuleSides);
        const glm::vec2 radial(
            std::cos(angle) * circumradius,
            std::sin(angle) * circumradius
        );
        capsulePoints.push_back(startCenter + centerOffset + radial);
        capsulePoints.push_back(endCenter + centerOffset + radial);
    }
    const std::vector<glm::vec2> capsule = buildConvexHull(
        std::move(capsulePoints)
    );
    return convexFootprintInsideWalkableSurface(
        runtime,
        capsule,
        from.y,
        candidateCells
    );
}

std::string canonicalPolygonKey(const std::vector<glm::vec2>& vertices) {
    std::vector<glm::vec2> normalized = normalizePolygonVertices(vertices);
    if (!polygonHasArea(normalized)) {
        return {};
    }

    std::vector<QuantizedVec2> quantized{};
    quantized.reserve(normalized.size());
    for (const glm::vec2& vertex : normalized) {
        quantized.push_back(quantizeVec2(vertex));
    }

    std::size_t bestStart = 0u;
    for (std::size_t candidate = 1u; candidate < quantized.size(); ++candidate) {
        for (std::size_t offset = 0u; offset < quantized.size(); ++offset) {
            const QuantizedVec2& lhs = quantized[(candidate + offset) % quantized.size()];
            const QuantizedVec2& rhs = quantized[(bestStart + offset) % quantized.size()];
            if (lhs == rhs) {
                continue;
            }
            if (lhs < rhs) {
                bestStart = candidate;
            }
            break;
        }
    }

    std::ostringstream key{};
    for (std::size_t offset = 0u; offset < quantized.size(); ++offset) {
        const QuantizedVec2& vertex = quantized[(bestStart + offset) % quantized.size()];
        key << vertex.x << "," << vertex.y << ";";
    }
    return key.str();
}

std::vector<glm::vec2> buildConvexHull(std::vector<glm::vec2> points) {
    if (points.size() < 3u) {
        return {};
    }

    std::sort(points.begin(), points.end(), [](const glm::vec2& lhs, const glm::vec2& rhs) {
        if (lhs.x != rhs.x) {
            return lhs.x < rhs.x;
        }
        return lhs.y < rhs.y;
    });
    points.erase(std::unique(points.begin(), points.end(), [](const glm::vec2& lhs, const glm::vec2& rhs) {
        return nearlyEqualVec2(lhs, rhs);
    }), points.end());
    if (points.size() < 3u) {
        return {};
    }

    std::vector<glm::vec2> lower{};
    for (const glm::vec2& point : points) {
        while (lower.size() >= 2u &&
               triArea2(lower[lower.size() - 2u], lower.back(), point) <= kPolygonEpsilon) {
            lower.pop_back();
        }
        lower.push_back(point);
    }

    std::vector<glm::vec2> upper{};
    for (auto it = points.rbegin(); it != points.rend(); ++it) {
        while (upper.size() >= 2u &&
               triArea2(upper[upper.size() - 2u], upper.back(), *it) <= kPolygonEpsilon) {
            upper.pop_back();
        }
        upper.push_back(*it);
    }

    lower.pop_back();
    upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return normalizePolygonVertices(lower);
}

std::vector<SharedPortalResult> sharedBoundaryPortals(
    const NavRuntimeCell& lhs,
    const NavRuntimeCell& rhs
) {
    std::vector<SharedPortalResult> portals{};
    for (std::size_t lhsIndex = 0; lhsIndex < lhs.verticesXZ.size(); ++lhsIndex) {
        const glm::vec2& lhsA = lhs.verticesXZ[lhsIndex];
        const glm::vec2& lhsB = lhs.verticesXZ[(lhsIndex + 1u) % lhs.verticesXZ.size()];
        const glm::vec2 lhsEdge = lhsB - lhsA;
        const float lhsLength = glm::length(lhsEdge);
        if (lhsLength <= kPolygonEpsilon) {
            continue;
        }
        const glm::vec2 axis = lhsEdge / lhsLength;

        for (std::size_t rhsIndex = 0; rhsIndex < rhs.verticesXZ.size(); ++rhsIndex) {
            const glm::vec2& rhsA = rhs.verticesXZ[rhsIndex];
            const glm::vec2& rhsB = rhs.verticesXZ[(rhsIndex + 1u) % rhs.verticesXZ.size()];
            const glm::vec2 rhsEdge = rhsB - rhsA;
            const float rhsLength = glm::length(rhsEdge);
            if (rhsLength <= kPolygonEpsilon ||
                std::abs(cross2(axis, rhsEdge / rhsLength)) > kPolygonEpsilon ||
                std::abs(cross2(axis, rhsA - lhsA)) > kPolygonEpsilon) {
                continue;
            }

            const float rhsStart = glm::dot(rhsA - lhsA, axis);
            const float rhsEnd = glm::dot(rhsB - lhsA, axis);
            const float overlapStart = std::max(0.0f, std::min(rhsStart, rhsEnd));
            const float overlapEnd = std::min(lhsLength, std::max(rhsStart, rhsEnd));
            // Sub-millimetric overlaps are numerical slivers, not useful
            // traversable portals. Keeping them can make several faces appear
            // incident to the same edge after the conforming Polyanya split.
            if (overlapEnd - overlapStart <= kPortalBroadPhaseEpsilon) {
                continue;
            }
            portals.push_back(SharedPortalResult{
                lhsA + axis * overlapStart,
                lhsA + axis * overlapEnd
            });
        }
    }
    return portals;
}

std::optional<SharedPortalResult> sharedBoundaryPortal(
    const NavRuntimeCell& lhs,
    const NavRuntimeCell& rhs
) {
    std::vector<SharedPortalResult> portals =
        sharedBoundaryPortals(lhs, rhs);
    if (portals.empty()) {
        return std::nullopt;
    }
    return portals.front();
}

std::optional<std::vector<glm::vec2>> tryMergeConvexCells(
    const NavRuntimeCell& lhs,
    const NavRuntimeCell& rhs
) {
    if (std::abs(lhs.elevationY - rhs.elevationY) > kLayerGroupingEpsilon) {
        return std::nullopt;
    }
    if (!sharedBoundaryPortal(lhs, rhs).has_value()) {
        return std::nullopt;
    }

    std::vector<glm::vec2> points = lhs.verticesXZ;
    for (const glm::vec2& vertex : rhs.verticesXZ) {
        if (std::find_if(points.begin(), points.end(), [&](const glm::vec2& existing) {
                return nearlyEqualVec2(existing, vertex);
            }) == points.end()) {
            points.push_back(vertex);
        }
    }

    std::vector<glm::vec2> hull = buildConvexHull(points);
    if (!polygonHasArea(hull)) {
        return std::nullopt;
    }

    // The monotone hull is assembled from float geometry. With very long
    // world-boundary edges, float cross products can retain a point that is
    // microscopically inside the true double-precision hull. Polyanya performs
    // its predicates in double, so remove those non-left turns before a merged
    // cell becomes part of the runtime mesh.
    bool removedImpreciseTurn = true;
    while (removedImpreciseTurn && hull.size() > 3u) {
        removedImpreciseTurn = false;
        for (std::size_t index = 0u; index < hull.size(); ++index) {
            if (preciseTriArea2(
                    hull[(index + hull.size() - 1u) % hull.size()],
                    hull[index],
                    hull[(index + 1u) % hull.size()]) <=
                static_cast<double>(kPolygonEpsilon)) {
                hull.erase(
                    hull.begin() + static_cast<std::ptrdiff_t>(index));
                removedImpreciseTurn = true;
                break;
            }
        }
    }

    const float mergedArea = std::abs(polygonSignedArea(hull));
    const float inputArea = std::abs(polygonSignedArea(lhs.verticesXZ)) + std::abs(polygonSignedArea(rhs.verticesXZ));
    if (std::abs(mergedArea - inputArea) > 0.001f) {
        return std::nullopt;
    }
    return hull;
}

void mergeAdjacentConvexCellsInternal(
    std::vector<NavRuntimeCell>& cells,
    std::vector<std::vector<std::size_t>>* cellToPolygonIndices
) {
    if (cells.size() < 2u) {
        return;
    }
    if (cellToPolygonIndices != nullptr && cellToPolygonIndices->size() != cells.size()) {
        return;
    }

    struct CellBounds {
        glm::vec2 minXZ{0.0f};
        glm::vec2 maxXZ{0.0f};
    };
    struct MergeCandidate {
        std::size_t lhs{0u};
        std::size_t rhs{0u};
        std::uint64_t lhsVersion{0u};
        std::uint64_t rhsVersion{0u};
        float portalLength{0.0f};
        std::vector<glm::vec2> mergedVertices{};

        bool operator<(const MergeCandidate& other) const {
            if (mergedVertices.size() != other.mergedVertices.size()) {
                return mergedVertices.size() > other.mergedVertices.size();
            }
            if (portalLength != other.portalLength) {
                return portalLength < other.portalLength;
            }
            if (lhs != other.lhs) {
                return lhs > other.lhs;
            }
            return rhs > other.rhs;
        }
    };
    struct MergeCandidatePriority {
        bool preserveScanOrder{false};

        bool operator()(const MergeCandidate& lhs, const MergeCandidate& rhs) const {
            if (!preserveScanOrder) {
                return lhs < rhs;
            }
            if (lhs.lhs != rhs.lhs) {
                return lhs.lhs > rhs.lhs;
            }
            return lhs.rhs > rhs.rhs;
        }
    };

    std::vector<std::uint8_t> active(cells.size(), 1u);
    std::vector<std::uint64_t> versions(cells.size(), 0u);
    std::vector<CellBounds> bounds{};
    bounds.reserve(cells.size());
    for (const NavRuntimeCell& cell : cells) {
        const auto [minXZ, maxXZ] = polygonBoundsXZ(cell.verticesXZ);
        bounds.push_back(CellBounds{minXZ, maxXZ});
    }

    std::priority_queue<
        MergeCandidate,
        std::vector<MergeCandidate>,
        MergeCandidatePriority
    > candidates(MergeCandidatePriority{cellToPolygonIndices != nullptr});
    const auto enqueueCandidate = [&](std::size_t lhsIndex, std::size_t rhsIndex) {
        if (lhsIndex == rhsIndex || active[lhsIndex] == 0u || active[rhsIndex] == 0u) {
            return;
        }
        if (lhsIndex > rhsIndex) {
            std::swap(lhsIndex, rhsIndex);
        }
        if (cellToPolygonIndices != nullptr &&
            (*cellToPolygonIndices)[lhsIndex] != (*cellToPolygonIndices)[rhsIndex]) {
            return;
        }
        if (!boundsOverlapXZ(
                bounds[lhsIndex].minXZ,
                bounds[lhsIndex].maxXZ,
                bounds[rhsIndex].minXZ,
                bounds[rhsIndex].maxXZ)) {
            return;
        }
        const auto portal = sharedBoundaryPortal(cells[lhsIndex], cells[rhsIndex]);
        if (!portal.has_value()) {
            return;
        }
        auto merged = tryMergeConvexCells(cells[lhsIndex], cells[rhsIndex]);
        if (!merged.has_value()) {
            return;
        }
        candidates.push(MergeCandidate{
            lhsIndex,
            rhsIndex,
            versions[lhsIndex],
            versions[rhsIndex],
            glm::length(portal->b - portal->a),
            std::move(*merged)
        });
    };

    std::vector<std::size_t> sweepOrder(cells.size());
    for (std::size_t index = 0; index < cells.size(); ++index) {
        sweepOrder[index] = index;
    }
    std::sort(sweepOrder.begin(), sweepOrder.end(), [&](std::size_t lhs, std::size_t rhs) {
        if (bounds[lhs].minXZ.x != bounds[rhs].minXZ.x) {
            return bounds[lhs].minXZ.x < bounds[rhs].minXZ.x;
        }
        return lhs < rhs;
    });
    for (std::size_t orderIndex = 0; orderIndex < sweepOrder.size(); ++orderIndex) {
        const std::size_t lhsIndex = sweepOrder[orderIndex];
        for (std::size_t candidateOrder = orderIndex + 1u; candidateOrder < sweepOrder.size(); ++candidateOrder) {
            const std::size_t rhsIndex = sweepOrder[candidateOrder];
            if (bounds[rhsIndex].minXZ.x > bounds[lhsIndex].maxXZ.x + kPolygonEpsilon) {
                break;
            }
            enqueueCandidate(lhsIndex, rhsIndex);
        }
    }

    while (!candidates.empty()) {
        MergeCandidate candidate = candidates.top();
        candidates.pop();
        if (active[candidate.lhs] == 0u || active[candidate.rhs] == 0u ||
            versions[candidate.lhs] != candidate.lhsVersion ||
            versions[candidate.rhs] != candidate.rhsVersion) {
            continue;
        }

        const auto [mergedMinXZ, mergedMaxXZ] =
            polygonBoundsXZ(candidate.mergedVertices);
        bool overlapsThirdCell = false;
        for (std::size_t otherIndex = 0u;
             otherIndex < cells.size();
             ++otherIndex) {
            if (otherIndex == candidate.lhs ||
                otherIndex == candidate.rhs ||
                active[otherIndex] == 0u ||
                !boundsOverlapXZ(
                    mergedMinXZ,
                    mergedMaxXZ,
                    bounds[otherIndex].minXZ,
                    bounds[otherIndex].maxXZ)) {
                continue;
            }
            if (convexPolygonsHaveInteriorOverlap(
                    candidate.mergedVertices,
                    cells[otherIndex].verticesXZ)) {
                overlapsThirdCell = true;
                break;
            }
        }
        if (overlapsThirdCell) {
            continue;
        }

        cells[candidate.lhs].verticesXZ = std::move(candidate.mergedVertices);
        ++versions[candidate.lhs];
        active[candidate.rhs] = 0u;
        ++versions[candidate.rhs];
        const auto [minXZ, maxXZ] = polygonBoundsXZ(cells[candidate.lhs].verticesXZ);
        bounds[candidate.lhs] = CellBounds{minXZ, maxXZ};

        for (std::size_t otherIndex = 0; otherIndex < cells.size(); ++otherIndex) {
            enqueueCandidate(candidate.lhs, otherIndex);
        }
    }

    std::vector<NavRuntimeCell> compacted{};
    compacted.reserve(cells.size());
    std::vector<std::vector<std::size_t>> compactedMemberships{};
    if (cellToPolygonIndices != nullptr) {
        compactedMemberships.reserve(cellToPolygonIndices->size());
    }
    for (std::size_t index = 0; index < cells.size(); ++index) {
        if (active[index] != 0u) {
            compacted.push_back(std::move(cells[index]));
            if (cellToPolygonIndices != nullptr) {
                compactedMemberships.push_back(std::move((*cellToPolygonIndices)[index]));
            }
        }
    }
    cells = std::move(compacted);
    if (cellToPolygonIndices != nullptr) {
        *cellToPolygonIndices = std::move(compactedMemberships);
    }
}

void mergeAdjacentConvexCells(std::vector<NavRuntimeCell>& cells) {
    mergeAdjacentConvexCellsInternal(cells, nullptr);
}

std::vector<NavRuntimeCell> bakeLayerRuntimeCells(
    const BakeLayerData& layer,
    std::vector<std::vector<std::size_t>>& outCellToPolygonIndices
) {
    // Build the geometric union incrementally.  The previous implementation
    // extended every triangle edge into an infinite split line and applied all
    // of those lines to every triangle.  Tiny overlaps introduced by asset
    // serialization therefore turned a few dozen authored polygons into
    // thousands of runtime cells.
    std::vector<NavRuntimeCell> cells{};
    for (const BakedTriangle& triangle : layer.triangles) {
        appendConvexPolygonToUnion(
            cells,
            {
                triangle.verticesXZ[0],
                triangle.verticesXZ[1],
                triangle.verticesXZ[2],
            },
            layer.elevationY
        );
    }
    mergeAdjacentConvexCells(cells);

    // Normalize once after the first greedy pass. Imported/generated input can
    // already contain overlapping convex pieces; the guarded merge above can
    // only preserve disjointness when its input is a partition.
    std::vector<NavRuntimeCell> disjointCells{};
    disjointCells.reserve(cells.size());
    for (const NavRuntimeCell& cell : cells) {
        appendConvexPolygonToUnion(
            disjointCells,
            cell.verticesXZ,
            layer.elevationY
        );
    }
    mergeAdjacentConvexCells(disjointCells);
    cells = std::move(disjointCells);
    if (convexCellSetHasInteriorOverlap(cells)) {
        std::vector<NavRuntimeCell> finalDisjointCells{};
        finalDisjointCells.reserve(cells.size());
        for (const NavRuntimeCell& cell : cells) {
            appendConvexPolygonToUnion(
                finalDisjointCells,
                cell.verticesXZ,
                layer.elevationY
            );
        }
        cells = std::move(finalDisjointCells);
    }

    // Membership is metadata used by authored links and by clearance handling.
    // A runtime cell can cross an authored-polygon boundary after union/merge,
    // so determine membership from positive-area triangle intersections rather
    // than from the centroid alone.
    outCellToPolygonIndices.clear();
    outCellToPolygonIndices.resize(cells.size());
    for (std::size_t cellIndex = 0u; cellIndex < cells.size(); ++cellIndex) {
        std::vector<std::size_t>& memberships = outCellToPolygonIndices[cellIndex];
        for (const BakedTriangle& triangle : layer.triangles) {
            const std::vector<glm::vec2> triangleVertices{
                triangle.verticesXZ[0],
                triangle.verticesXZ[1],
                triangle.verticesXZ[2],
            };
            if (!polygonHasArea(clipConvexPolygons(
                    cells[cellIndex].verticesXZ,
                    triangleVertices))) {
                continue;
            }
            if (std::find(
                    memberships.begin(),
                    memberships.end(),
                    triangle.authoredPolygonIndex) == memberships.end()) {
                memberships.push_back(triangle.authoredPolygonIndex);
            }
        }
        std::sort(memberships.begin(), memberships.end());
    }
    return cells;
}

std::vector<NavRuntimeCell> bakeDisjointLayerRuntimeCells(
    const BakeLayerData& layer,
    std::vector<std::vector<std::size_t>>& outCellToPolygonIndices
) {
    std::vector<NavRuntimeCell> cells{};
    for (const AuthoredBakePolygon& polygon : layer.polygons) {
        std::vector<NavRuntimeCell> polygonCells{};
        for (const BakedTriangle& triangle : layer.triangles) {
            if (triangle.authoredPolygonIndex != polygon.assetIndex) {
                continue;
            }
            polygonCells.push_back(NavRuntimeCell{
                layer.elevationY,
                {
                    triangle.verticesXZ[0],
                    triangle.verticesXZ[1],
                    triangle.verticesXZ[2],
                }
            });
        }
        mergeAdjacentConvexCells(polygonCells);
        for (NavRuntimeCell& cell : polygonCells) {
            cells.push_back(std::move(cell));
            outCellToPolygonIndices.push_back({polygon.assetIndex});
        }
    }
    return cells;
}

bool bakeLayerHasInteriorPolygonOverlap(const BakeLayerData& layer) {
    struct TriangleGeometry {
        std::size_t authoredPolygonIndex{0u};
        std::vector<glm::vec2> verticesXZ{};
        glm::vec2 minXZ{0.0f};
        glm::vec2 maxXZ{0.0f};
    };

    std::vector<TriangleGeometry> triangles{};
    triangles.reserve(layer.triangles.size());
    for (const BakedTriangle& triangle : layer.triangles) {
        std::vector<glm::vec2> vertices{
            triangle.verticesXZ[0],
            triangle.verticesXZ[1],
            triangle.verticesXZ[2],
        };
        const auto [minXZ, maxXZ] = polygonBoundsXZ(vertices);
        triangles.push_back(TriangleGeometry{
            triangle.authoredPolygonIndex,
            std::move(vertices),
            minXZ,
            maxXZ
        });
    }

    std::vector<std::size_t> sweepOrder(triangles.size());
    for (std::size_t index = 0u; index < sweepOrder.size(); ++index) {
        sweepOrder[index] = index;
    }
    std::sort(sweepOrder.begin(), sweepOrder.end(), [&](std::size_t lhs, std::size_t rhs) {
        if (triangles[lhs].minXZ.x != triangles[rhs].minXZ.x) {
            return triangles[lhs].minXZ.x < triangles[rhs].minXZ.x;
        }
        return lhs < rhs;
    });

    for (std::size_t orderIndex = 0u; orderIndex < sweepOrder.size(); ++orderIndex) {
        const TriangleGeometry& lhs = triangles[sweepOrder[orderIndex]];
        for (std::size_t candidateOrder = orderIndex + 1u; candidateOrder < sweepOrder.size(); ++candidateOrder) {
            const TriangleGeometry& rhs = triangles[sweepOrder[candidateOrder]];
            if (rhs.minXZ.x > lhs.maxXZ.x + kPolygonEpsilon) {
                break;
            }
            if (lhs.authoredPolygonIndex == rhs.authoredPolygonIndex ||
                !boundsOverlapXZ(lhs.minXZ, lhs.maxXZ, rhs.minXZ, rhs.maxXZ)) {
                continue;
            }
            if (convexPolygonsHaveInteriorOverlap(
                    lhs.verticesXZ,
                    rhs.verticesXZ)) {
                return true;
            }
        }
    }
    return false;
}

bool runtimeCellsHaveInteriorOverlap(
    const std::vector<NavRuntimeCell>& cells,
    const std::vector<glm::vec2>& cellMinXZ,
    const std::vector<glm::vec2>& cellMaxXZ
) {
    if (cellMinXZ.size() != cells.size() || cellMaxXZ.size() != cells.size()) {
        return true;
    }
    std::vector<std::size_t> sweepOrder(cells.size());
    for (std::size_t index = 0u; index < sweepOrder.size(); ++index) {
        sweepOrder[index] = index;
    }
    std::sort(
        sweepOrder.begin(),
        sweepOrder.end(),
        [&](std::size_t lhs, std::size_t rhs) {
            return cellMinXZ[lhs].x < cellMinXZ[rhs].x;
        }
    );
    for (std::size_t orderIndex = 0u;
         orderIndex < sweepOrder.size();
         ++orderIndex) {
        const std::size_t lhs = sweepOrder[orderIndex];
        for (std::size_t candidateOrder = orderIndex + 1u;
             candidateOrder < sweepOrder.size();
             ++candidateOrder) {
            const std::size_t rhs = sweepOrder[candidateOrder];
            if (cellMinXZ[rhs].x > cellMaxXZ[lhs].x + kPolygonEpsilon) {
                break;
            }
            if (std::abs(cells[lhs].elevationY - cells[rhs].elevationY) >
                    kLayerGroupingEpsilon ||
                !boundsOverlapXZ(
                    cellMinXZ[lhs],
                    cellMaxXZ[lhs],
                    cellMinXZ[rhs],
                    cellMaxXZ[rhs])) {
                continue;
            }
            if (convexPolygonsHaveInteriorOverlap(
                    cells[lhs].verticesXZ,
                    cells[rhs].verticesXZ)) {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::size_t> findContainingCells(const NavigationRuntime& runtime, const glm::vec3& point) {
    std::vector<std::size_t> containing{};
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t index = 0; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        if (std::abs(point.y - cell.elevationY) > kLayerGroupingEpsilon) {
            continue;
        }
        if (index < runtime.bakedCellMinXZ.size() &&
            index < runtime.bakedCellMaxXZ.size() &&
            (pointXZ.x < runtime.bakedCellMinXZ[index].x - kPolygonEpsilon ||
             pointXZ.x > runtime.bakedCellMaxXZ[index].x + kPolygonEpsilon ||
             pointXZ.y < runtime.bakedCellMinXZ[index].y - kPolygonEpsilon ||
             pointXZ.y > runtime.bakedCellMaxXZ[index].y + kPolygonEpsilon)) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            containing.push_back(index);
        }
    }
    return containing;
}

bool containsCellIndex(const std::vector<std::size_t>& indices, std::size_t cellIndex) {
    return std::find(indices.begin(), indices.end(), cellIndex) != indices.end();
}

std::optional<std::size_t> findNearestCell(const NavigationRuntime& runtime, const glm::vec3& point) {
    std::optional<std::size_t> bestIndex{};
    float bestDistance = std::numeric_limits<float>::max();
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t index = 0; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        const glm::vec2 closest =
            closestPointOnPolygonXZ(pointXZ, cell.verticesXZ);
        const float planarDistance = glm::distance(closest, pointXZ);
        const float verticalDistance = std::abs(point.y - cell.elevationY);
        const float score = planarDistance + verticalDistance * 2.0f;
        if (score < bestDistance) {
            bestDistance = score;
            bestIndex = index;
        }
    }
    return bestIndex;
}

std::vector<std::size_t> findLinkEndpointCells(
    const NavigationRuntime& runtime,
    std::size_t authoredPolygonIndex,
    const glm::vec3& point
) {
    std::vector<std::size_t> containing{};
    if (authoredPolygonIndex >= runtime.polygonToCellIndices.size()) {
        return containing;
    }

    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t cellIndex : runtime.polygonToCellIndices[authoredPolygonIndex]) {
        if (cellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        if (std::abs(point.y - cell.elevationY) > kLayerGroupingEpsilon) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            containing.push_back(cellIndex);
        }
    }
    return containing;
}

std::optional<std::size_t> findNearestCandidateCell(
    const NavigationRuntime& runtime,
    const std::vector<std::size_t>& candidates,
    const glm::vec3& point
) {
    std::optional<std::size_t> bestIndex{};
    float bestScore = std::numeric_limits<float>::max();
    for (std::size_t cellIndex : candidates) {
        if (cellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const glm::vec2 center = cellIndex < runtime.bakedCellCenters.size()
            ? runtime.bakedCellCenters[cellIndex]
            : polygonCentroidXZ(runtime.bakedCells[cellIndex].verticesXZ);
        const float planarDistance = glm::distance(center, glm::vec2(point.x, point.z));
        const float verticalDistance = std::abs(point.y - runtime.bakedCells[cellIndex].elevationY);
        const float score = planarDistance + verticalDistance * 2.0f;
        if (score < bestScore) {
            bestScore = score;
            bestIndex = cellIndex;
        }
    }
    return bestIndex;
}

bool pointInsideAuthoredWalkableSurface(const NavigationRuntime& runtime, const glm::vec3& point) {
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t index = 0u; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        if (std::abs(point.y - cell.elevationY) > kLayerGroupingEpsilon) {
            continue;
        }
        if (index < runtime.bakedCellMinXZ.size() && index < runtime.bakedCellMaxXZ.size() &&
            (pointXZ.x < runtime.bakedCellMinXZ[index].x - kPolygonEpsilon ||
             pointXZ.x > runtime.bakedCellMaxXZ[index].x + kPolygonEpsilon ||
             pointXZ.y < runtime.bakedCellMinXZ[index].y - kPolygonEpsilon ||
             pointXZ.y > runtime.bakedCellMaxXZ[index].y + kPolygonEpsilon)) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            return true;
        }
    }
    return false;
}

template <typename RuntimeView>
bool segmentInsideBakedWalkableSurface(
    const RuntimeView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const std::vector<std::size_t>* candidateCells
) {
    if (std::abs(from.y - to.y) > kLayerGroupingEpsilon) {
        return false;
    }

    const glm::vec2 segmentStart(from.x, from.z);
    const glm::vec2 segmentEnd(to.x, to.z);
    const glm::vec2 segmentDelta = segmentEnd - segmentStart;
    if (glm::dot(segmentDelta, segmentDelta) <= kPlaneEpsilon * kPlaneEpsilon) {
        if (candidateCells != nullptr) {
            for (std::size_t cellIndex : *candidateCells) {
                if (cellIndex >= runtime.bakedCells.size()) {
                    continue;
                }
                const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
                if (std::abs(from.y - cell.elevationY) <=
                        kLayerGroupingEpsilon &&
                    pointInOrOnPolygonXZ(segmentStart, cell.verticesXZ)) {
                    return true;
                }
            }
            return false;
        }
        return pointInsideAuthoredWalkableSurface(runtime, from);
    }

    const glm::vec2 segmentMin = glm::min(segmentStart, segmentEnd);
    const glm::vec2 segmentMax = glm::max(segmentStart, segmentEnd);
    // This predicate is the hot inner loop of clearance visibility searches.
    // Reusing one scratch buffer per worker avoids thousands of heap
    // allocations per request while remaining safe for concurrent tasks.
    thread_local std::vector<std::pair<float, float>> coveredIntervals{};
    coveredIntervals.clear();
    const std::size_t candidateCount = candidateCells != nullptr
        ? candidateCells->size()
        : runtime.bakedCells.size();
    if (coveredIntervals.capacity() < candidateCount) {
        coveredIntervals.reserve(candidateCount);
    }
    const auto collectCoveredInterval = [&](std::size_t cellIndex) {
        if (cellIndex >= runtime.bakedCells.size()) {
            return;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        if (std::abs(from.y - cell.elevationY) > kLayerGroupingEpsilon) {
            return;
        }
        if (cellIndex < runtime.bakedCellMinXZ.size() && cellIndex < runtime.bakedCellMaxXZ.size() &&
            !boundsOverlapXZ(
                segmentMin,
                segmentMax,
                runtime.bakedCellMinXZ[cellIndex],
                runtime.bakedCellMaxXZ[cellIndex])) {
            return;
        }

        const float orientation = polygonSignedArea(cell.verticesXZ) >= 0.0f ? 1.0f : -1.0f;
        float entryT = 0.0f;
        float exitT = 1.0f;
        bool intersects = true;
        for (std::size_t edgeIndex = 0u; edgeIndex < cell.verticesXZ.size(); ++edgeIndex) {
            const glm::vec2& edgeStart = cell.verticesXZ[edgeIndex];
            const glm::vec2& edgeEnd = cell.verticesXZ[(edgeIndex + 1u) % cell.verticesXZ.size()];
            const glm::vec2 edge = edgeEnd - edgeStart;
            const float startSide = orientation * cross2(edge, segmentStart - edgeStart);
            const float sideDelta = orientation * cross2(edge, segmentDelta);
            if (std::abs(sideDelta) <= kPlaneEpsilon) {
                if (startSide < -kPolygonEpsilon) {
                    intersects = false;
                    break;
                }
                continue;
            }

            const float boundaryT = (-kPolygonEpsilon - startSide) / sideDelta;
            if (sideDelta > 0.0f) {
                entryT = std::max(entryT, boundaryT);
            } else {
                exitT = std::min(exitT, boundaryT);
            }
            if (entryT > exitT + kPolygonEpsilon) {
                intersects = false;
                break;
            }
        }
        if (intersects && exitT >= -kPolygonEpsilon && entryT <= 1.0f + kPolygonEpsilon) {
            coveredIntervals.emplace_back(
                std::clamp(entryT, 0.0f, 1.0f),
                std::clamp(exitT, 0.0f, 1.0f)
            );
        }
    };
    if (candidateCells != nullptr) {
        for (std::size_t cellIndex : *candidateCells) {
            collectCoveredInterval(cellIndex);
        }
    } else {
        for (std::size_t cellIndex = 0u;
             cellIndex < runtime.bakedCells.size();
             ++cellIndex) {
            collectCoveredInterval(cellIndex);
        }
    }

    std::sort(coveredIntervals.begin(), coveredIntervals.end());
    float coveredUntil = 0.0f;
    for (const auto& [entryT, exitT] : coveredIntervals) {
        if (exitT < coveredUntil - kPolygonEpsilon) {
            continue;
        }
        if (entryT > coveredUntil + kPolygonEpsilon) {
            return false;
        }
        coveredUntil = std::max(coveredUntil, exitT);
        if (coveredUntil >= 1.0f - kPolygonEpsilon) {
            return true;
        }
    }
    return false;
}

bool segmentInsideAuthoredWalkableSurface(
    const NavigationRuntime& runtime,
    const glm::vec3& from,
    const glm::vec3& to
) {
    return segmentInsideBakedWalkableSurface(runtime, from, to);
}

// --- NavigationSolveView overloads for pathfinding pipeline ---

glm::vec3 cellCenter3(const NavigationSolveView& runtime, std::size_t cellIndex) {
    const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
    const glm::vec2 center = cellIndex < runtime.bakedCellCenters.size()
        ? runtime.bakedCellCenters[cellIndex]
        : polygonCentroidXZ(cell.verticesXZ);
    return glm::vec3(center.x, cell.elevationY, center.y);
}

std::vector<std::size_t> findContainingCells(const NavigationSolveView& runtime, const glm::vec3& point) {
    std::vector<std::size_t> containing{};
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t index = 0; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        if (std::abs(point.y - cell.elevationY) > kLayerGroupingEpsilon) {
            continue;
        }
        if (index < runtime.bakedCellMinXZ.size() &&
            index < runtime.bakedCellMaxXZ.size() &&
            (pointXZ.x < runtime.bakedCellMinXZ[index].x - kPolygonEpsilon ||
             pointXZ.x > runtime.bakedCellMaxXZ[index].x + kPolygonEpsilon ||
             pointXZ.y < runtime.bakedCellMinXZ[index].y - kPolygonEpsilon ||
             pointXZ.y > runtime.bakedCellMaxXZ[index].y + kPolygonEpsilon)) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            containing.push_back(index);
        }
    }
    return containing;
}

std::optional<std::size_t> findNearestCell(const NavigationSolveView& runtime, const glm::vec3& point) {
    std::optional<std::size_t> bestIndex{};
    float bestDistance = std::numeric_limits<float>::max();
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t index = 0; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        const glm::vec2 closest =
            closestPointOnPolygonXZ(pointXZ, cell.verticesXZ);
        const float planarDistance = glm::distance(closest, pointXZ);
        const float verticalDistance = std::abs(point.y - cell.elevationY);
        const float score = planarDistance + verticalDistance * 2.0f;
        if (score < bestDistance) {
            bestDistance = score;
            bestIndex = index;
        }
    }
    return bestIndex;
}

std::optional<std::size_t> findPolygonIndexById(const NavigationSolveView& runtime, int polygonId) {
    const auto it = runtime.polygonIndexById.find(polygonId);
    if (it == runtime.polygonIndexById.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::size_t> findLinkEndpointCells(
    const NavigationSolveView& runtime,
    std::size_t authoredPolygonIndex,
    const glm::vec3& point
) {
    std::vector<std::size_t> containing{};
    if (authoredPolygonIndex >= runtime.polygonToCellIndices.size()) {
        return containing;
    }

    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t cellIndex : runtime.polygonToCellIndices[authoredPolygonIndex]) {
        if (cellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        if (std::abs(point.y - cell.elevationY) > kLayerGroupingEpsilon) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            containing.push_back(cellIndex);
        }
    }
    return containing;
}

std::optional<std::size_t> findNearestCandidateCell(
    const NavigationSolveView& runtime,
    const std::vector<std::size_t>& candidates,
    const glm::vec3& point
) {
    std::optional<std::size_t> bestIndex{};
    float bestScore = std::numeric_limits<float>::max();
    for (std::size_t cellIndex : candidates) {
        if (cellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const glm::vec2 center = cellIndex < runtime.bakedCellCenters.size()
            ? runtime.bakedCellCenters[cellIndex]
            : polygonCentroidXZ(runtime.bakedCells[cellIndex].verticesXZ);
        const float planarDistance = glm::distance(center, glm::vec2(point.x, point.z));
        const float verticalDistance = std::abs(point.y - runtime.bakedCells[cellIndex].elevationY);
        const float score = planarDistance + verticalDistance * 2.0f;
        if (score < bestScore) {
            bestScore = score;
            bestIndex = cellIndex;
        }
    }
    return bestIndex;
}

bool pointInsideAuthoredWalkableSurface(const NavigationSolveView& runtime, const glm::vec3& point) {
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t index = 0u; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        if (std::abs(point.y - cell.elevationY) > kLayerGroupingEpsilon) {
            continue;
        }
        if (index < runtime.bakedCellMinXZ.size() && index < runtime.bakedCellMaxXZ.size() &&
            (pointXZ.x < runtime.bakedCellMinXZ[index].x - kPolygonEpsilon ||
             pointXZ.x > runtime.bakedCellMaxXZ[index].x + kPolygonEpsilon ||
             pointXZ.y < runtime.bakedCellMinXZ[index].y - kPolygonEpsilon ||
             pointXZ.y > runtime.bakedCellMaxXZ[index].y + kPolygonEpsilon)) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            return true;
        }
    }
    return false;
}

bool segmentInsideAuthoredWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to
) {
    return segmentInsideBakedWalkableSurface(runtime, from, to);
}

bool segmentInsideAuthoredWalkableSurfaceWithClearance(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile
) {
    if (profile.empty()) {
        return segmentInsideAuthoredWalkableSurface(runtime, from, to);
    }
    if (!segmentInsideAuthoredWalkableSurface(runtime, from, to)) {
        return false;
    }
    const glm::vec2 travelDirection = travelDirectionForSegment(from, to);
    if (profile.shape == AgentClearanceShape::Box) {
        return boxSweepInsideWalkableSurface(
            runtime,
            from,
            to,
            profile,
            travelDirection
        );
    }

    if (!visitClearanceBoundaryOffsets(
            profile,
            travelDirection,
            kSegmentClearanceSampleDirections,
            [&](const glm::vec2& planarOffset) {
                const glm::vec3 offset(
                    planarOffset.x,
                    0.0f,
                    planarOffset.y
                );
                return segmentInsideAuthoredWalkableSurface(
                    runtime,
                    from + offset,
                    to + offset
                );
            })) {
        return false;
    }
    return sphereSweepInsideWalkableSurface(
        runtime,
        from,
        to,
        profile,
        travelDirection
    );
}

bool segmentInsideSelectedWalkableCellsWithClearance(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile,
    const std::vector<std::size_t>& candidateCells
) {
    if (!segmentInsideBakedWalkableSurface(
            runtime,
            from,
            to,
            &candidateCells)) {
        return false;
    }
    const glm::vec2 travelDirection = travelDirectionForSegment(from, to);
    if (profile.shape == AgentClearanceShape::Box) {
        return boxSweepInsideWalkableSurface(
            runtime,
            from,
            to,
            profile,
            travelDirection,
            &candidateCells
        );
    }
    if (!visitClearanceBoundaryOffsets(
            profile,
            travelDirection,
            kSegmentClearanceSampleDirections,
            [&](const glm::vec2& planarOffset) {
                const glm::vec3 offset(
                    planarOffset.x,
                    0.0f,
                    planarOffset.y
                );
                return segmentInsideBakedWalkableSurface(
                    runtime,
                    from + offset,
                    to + offset,
                    &candidateCells
                );
            })) {
        return false;
    }
    return sphereSweepInsideWalkableSurface(
        runtime,
        from,
        to,
        profile,
        travelDirection,
        &candidateCells
    );
}

std::vector<glm::vec3> shortcutPathCorners(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const std::vector<glm::vec3>& corners,
    float arrivalRadius,
    const AgentClearanceProfile& profile
) {
    std::vector<glm::vec3> optimized{};
    glm::vec3 anchor = start;
    std::size_t index = 0u;
    while (index < corners.size()) {
        std::size_t bestReach = index;
        for (std::size_t candidate = corners.size(); candidate-- > index;) {
            if (segmentInsideAuthoredWalkableSurfaceWithClearance(
                    runtime, anchor, corners[candidate], profile)) {
                bestReach = candidate;
                break;
            }
        }
        appendPathCorner(optimized, corners[bestReach], arrivalRadius);
        anchor = corners[bestReach];
        index = bestReach + 1u;
    }
    return optimized;
}

// --- Pathfinding pipeline ---

std::optional<ResolvedPathEndpoints> resolvePathEndpoints(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const glm::vec3& destination,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled
) {
    const auto isCancelled = [&]() {
        return cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed);
    };
    if (isCancelled()) {
        return std::nullopt;
    }
    ResolvedPathEndpoints endpoints{};
    const glm::vec3 projectedStart = projectEndpointOntoWalkableLayer(
        runtime,
        start
    ).value_or(start);
    const glm::vec3 projectedDestination =
        projectEndpointOntoWalkableLayer(runtime, destination)
            .value_or(destination);
    if (!profile.empty()) {
        const glm::vec2 travelDir = travelDirectionForSegment(
            projectedStart,
            projectedDestination,
            glm::vec2(0.0f, 1.0f)
        );
        endpoints.resolvedStart = resolvePointWithClearance(
            runtime,
            projectedStart,
            profile,
            travelDir,
            cancelled
        ).value_or(projectedStart);
        if (isCancelled()) {
            return std::nullopt;
        }

        // Destination: prefer approach-line projection for natural corner clearance.
        // Walk from click along reverse-approach direction until a safe position is found,
        // then binary-search back to find the nearest safe point along that line.
        const glm::vec2 approachDir = -travelDir;
        if (pointInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                projectedDestination,
                profile,
                approachDir)) {
            endpoints.resolvedDestination = projectedDestination;
        } else {
            const float maxSearch = glm::distance(
                glm::vec2(projectedStart.x, projectedStart.z),
                glm::vec2(
                    projectedDestination.x,
                    projectedDestination.z)
            );
            const float nominalClearance = std::max(
                supportDistance(profile, glm::vec2(1.0f, 0.0f), approachDir),
                supportDistance(profile, glm::vec2(0.0f, 1.0f), approachDir)
            );
            const float step = std::max(nominalClearance * 0.25f, 0.05f);
            glm::vec3 safeFar{};
            bool foundSafe = false;
            for (float t = step; t <= maxSearch; t += step) {
                if (isCancelled()) {
                    return std::nullopt;
                }
                safeFar = glm::vec3(
                    projectedDestination.x + approachDir.x * t,
                    projectedDestination.y,
                    projectedDestination.z + approachDir.y * t
                );
                if (pointInsideAuthoredWalkableSurface(runtime, safeFar) &&
                    pointInsideAuthoredWalkableSurfaceWithClearance(runtime, safeFar, profile, approachDir)) {
                    foundSafe = true;
                    break;
                }
            }
            if (foundSafe) {
                float unsafeT = 0.0f;
                float safeT = 1.0f;
                for (int s = 0; s < kClearanceBinarySearchSteps; ++s) {
                    const float midT = (unsafeT + safeT) * 0.5f;
                    const glm::vec3 mid = projectedDestination +
                        (safeFar - projectedDestination) * midT;
                    if (pointInsideAuthoredWalkableSurface(runtime, mid) &&
                        pointInsideAuthoredWalkableSurfaceWithClearance(runtime, mid, profile, approachDir)) {
                        safeT = midT;
                    } else {
                        unsafeT = midT;
                    }
                }
                endpoints.resolvedDestination = projectedDestination +
                    (safeFar - projectedDestination) * safeT;
            } else {
                endpoints.resolvedDestination = resolvePointWithClearance(
                    runtime,
                    projectedDestination,
                    profile,
                    -travelDir,
                    cancelled
                ).value_or(projectedDestination);
            }
        }
    } else {
        endpoints.resolvedStart = projectedStart;
        endpoints.resolvedDestination = projectedDestination;
    }
    endpoints.rawStartCells = findContainingCells(runtime, start);
    endpoints.rawTargetCells = findContainingCells(runtime, destination);
    if (endpoints.rawStartCells.empty()) {
        endpoints.rawStartCells = findContainingCells(
            runtime,
            projectedStart
        );
    }
    if (endpoints.rawTargetCells.empty()) {
        endpoints.rawTargetCells = findContainingCells(
            runtime,
            projectedDestination
        );
    }
    endpoints.startCells = findContainingCells(runtime, endpoints.resolvedStart);
    endpoints.targetCells = findContainingCells(runtime, endpoints.resolvedDestination);
    if (endpoints.startCells.empty()) {
        endpoints.startCells = endpoints.rawStartCells;
    }
    if (endpoints.targetCells.empty()) {
        endpoints.targetCells = endpoints.rawTargetCells;
    }
    if (endpoints.startCells.empty()) {
        if (const auto nearest = findNearestCell(runtime, endpoints.resolvedStart); nearest.has_value()) {
            endpoints.startCells.push_back(*nearest);
        }
    }
    if (endpoints.targetCells.empty()) {
        if (const auto nearest = findNearestCell(runtime, endpoints.resolvedDestination); nearest.has_value()) {
            endpoints.targetCells.push_back(*nearest);
        }
    }
    if (endpoints.startCells.empty() || endpoints.targetCells.empty()) {
        return std::nullopt;
    }
    return endpoints;
}

bool canSolveDirectPath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const AgentClearanceProfile& profile
) {
    return segmentInsideAuthoredWalkableSurfaceWithClearance(
        runtime,
        endpoints.resolvedStart,
        endpoints.resolvedDestination,
        profile
    );
}

struct NavCorridorStep {
    std::size_t fromCellIndex{0u};
    std::size_t toCellIndex{0u};
    std::size_t edgeIndex{0u};
    std::optional<SharedPortalResult> safePortal{};
};

struct NavTraversalState {
    std::size_t fromCellIndex{0u};
    std::size_t toCellIndex{0u};
    std::size_t edgeIndex{0u};
    glm::vec3 anchor{0.0f};
    std::optional<SharedPortalResult> safePortal{};
    bool traversable{false};
};

enum class PortalSamplingMode {
    Midpoint,
    Geometry,
};

std::optional<SharedPortalResult> safePortalForTraversal(
    const NavigationSolveView& runtime,
    std::size_t fromCellIndex,
    const NavGraphEdge& edge,
    const AgentClearanceProfile& profile
) {
    if (edge.viaLink || edge.targetCellIndex >= runtime.bakedCells.size()) {
        return std::nullopt;
    }
    const SharedPortalResult rawPortal{edge.portalA, edge.portalB};
    if (profile.empty() ||
        portalIsInternalToSharedAuthoredPolygon(
            runtime,
            fromCellIndex,
            edge.targetCellIndex,
            rawPortal
        )) {
        return rawPortal;
    }
    if (profile.shape == AgentClearanceShape::Box ||
        profile.shape == AgentClearanceShape::Sphere) {
        const glm::vec2 delta = edge.portalB - edge.portalA;
        const float length = glm::length(delta);
        // The heading at a graph portal is not known until an actual segment
        // is tested. Use a superset of every heading's feasible interval here;
        // exact swept-box validation below rejects unsafe arcs. This keeps an
        // anisotropic or off-centre box from losing its only valid heading.
        const float minimumExtent =
            profile.shape == AgentClearanceShape::Sphere
            ? profile.sphereRadius
            : std::min(
                profile.boxHalfExtentsXZ.x,
                profile.boxHalfExtentsXZ.y
            );
        const float minimumInset = std::max(
            0.0f,
            minimumExtent - glm::length(profile.centerXZ)
        );
        if (length <= minimumInset * 2.0f + kPolygonEpsilon) {
            return std::nullopt;
        }
        const glm::vec2 direction = delta / length;
        return SharedPortalResult{
            edge.portalA + direction * minimumInset,
            edge.portalB - direction * minimumInset,
        };
    }
    return shrinkPortal(
        edge.portalA,
        edge.portalB,
        profile,
        runtime.bakedCellCenters[edge.targetCellIndex] -
            runtime.bakedCellCenters[fromCellIndex]
    );
}

std::optional<std::vector<NavCorridorStep>> findAStarCorridor(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled,
    PortalSamplingMode samplingMode,
    const std::vector<std::pair<std::size_t, std::size_t>>&
        blockedTraversals
) {
    if (runtime.bakedCells.empty() || runtime.graph.size() != runtime.bakedCells.size()) {
        return std::nullopt;
    }

    // A state is an arrival sample on one directed portal (or link).  Keeping
    // arrivals distinct avoids the classic centroid-A* bug where reaching a
    // large polygon through the wrong side hides a shorter corridor.  Portal
    // endpoints are essential: shortest paths bend at boundary vertices, while
    // using only a portal midpoint can rank the wrong corridor before funneling.
    std::size_t graphEdgeCount = 0u;
    for (const std::vector<NavGraphEdge>& edges : runtime.graph) {
        graphEdgeCount += edges.size();
    }
    std::vector<NavTraversalState> traversals{};
    traversals.reserve(graphEdgeCount * 5u);
    std::vector<std::vector<std::size_t>> outgoingTraversals(
        runtime.graph.size()
    );
    const glm::vec2 startXZ(
        endpoints.resolvedStart.x,
        endpoints.resolvedStart.z
    );
    const glm::vec2 destinationXZ(
        endpoints.resolvedDestination.x,
        endpoints.resolvedDestination.z
    );
    for (std::size_t fromCellIndex = 0u;
         fromCellIndex < runtime.graph.size();
         ++fromCellIndex) {
        for (std::size_t edgeIndex = 0u;
             edgeIndex < runtime.graph[fromCellIndex].size();
             ++edgeIndex) {
            if (std::find(
                    blockedTraversals.begin(),
                    blockedTraversals.end(),
                    std::pair{fromCellIndex, edgeIndex}
                ) != blockedTraversals.end()) {
                continue;
            }
            const NavGraphEdge& edge = runtime.graph[fromCellIndex][edgeIndex];
            if (edge.targetCellIndex >= runtime.bakedCells.size()) {
                continue;
            }
            if (edge.viaLink) {
                if (!pointInsideAuthoredWalkableSurface(
                        runtime,
                        edge.linkStartPoint) ||
                    !pointInsideAuthoredWalkableSurface(
                        runtime,
                        edge.linkEndPoint)) {
                    continue;
                }
                if (!profile.empty()) {
                    const glm::vec2 linkDirection =
                        travelDirectionForSegment(
                            edge.linkStartPoint,
                            edge.linkEndPoint
                        );
                    if (!pointInsideAuthoredWalkableSurfaceWithClearance(
                            runtime,
                            edge.linkStartPoint,
                            profile,
                            linkDirection) ||
                        !pointInsideAuthoredWalkableSurfaceWithClearance(
                            runtime,
                            edge.linkEndPoint,
                            profile,
                            linkDirection)) {
                        continue;
                    }
                }
                outgoingTraversals[fromCellIndex].push_back(
                    traversals.size()
                );
                traversals.push_back(NavTraversalState{
                    fromCellIndex,
                    edge.targetCellIndex,
                    edgeIndex,
                    edge.linkEndPoint,
                    std::nullopt,
                    true
                });
                continue;
            }
            const std::optional<SharedPortalResult> safePortal =
                safePortalForTraversal(
                    runtime,
                    fromCellIndex,
                    edge,
                    profile
                );
            if (!safePortal.has_value()) {
                continue;
            }

            const glm::vec2 midpoint =
                (safePortal->a + safePortal->b) * 0.5f;
            std::vector<glm::vec2> portalSamples{midpoint};
            if (samplingMode == PortalSamplingMode::Geometry) {
                portalSamples = {
                    safePortal->a,
                    midpoint,
                    safePortal->b,
                    closestPointOnSegmentXZ(
                        startXZ,
                        safePortal->a,
                        safePortal->b
                    ),
                    closestPointOnSegmentXZ(
                        destinationXZ,
                        safePortal->a,
                        safePortal->b
                    ),
                };
            }
            std::vector<glm::vec2> uniqueSamples{};
            uniqueSamples.reserve(portalSamples.size());
            for (const glm::vec2& sample : portalSamples) {
                if (std::none_of(
                        uniqueSamples.begin(),
                        uniqueSamples.end(),
                        [&](const glm::vec2& existing) {
                            return nearlyEqualVec2(existing, sample);
                        })) {
                    uniqueSamples.push_back(sample);
                }
            }
            for (const glm::vec2& sample : uniqueSamples) {
                outgoingTraversals[fromCellIndex].push_back(
                    traversals.size()
                );
                traversals.push_back(NavTraversalState{
                    fromCellIndex,
                    edge.targetCellIndex,
                    edgeIndex,
                    glm::vec3(
                        sample.x,
                        runtime.bakedCells[
                            edge.targetCellIndex
                        ].elevationY,
                        sample.y
                    ),
                    safePortal,
                    true
                });
            }
        }
    }
    const std::size_t traversalCount = traversals.size();
    const std::size_t stateCount =
        traversalCount + runtime.bakedCells.size();
    const std::size_t invalidState =
        std::numeric_limits<std::size_t>::max();

    std::vector<std::uint8_t> targetCells(runtime.bakedCells.size(), 0u);
    for (std::size_t targetCellIndex : endpoints.targetCells) {
        if (targetCellIndex < targetCells.size()) {
            targetCells[targetCellIndex] = 1u;
        }
    }

    struct QueueItem {
        float fScore{0.0f};
        float gScore{0.0f};
        std::size_t stateIndex{0u};

        bool operator<(const QueueItem& other) const {
            if (fScore != other.fScore) {
                return fScore > other.fScore;
            }
            return gScore < other.gScore;
        }
    };

    const auto stateCell = [&](std::size_t stateIndex) {
        return stateIndex < traversalCount
            ? traversals[stateIndex].toCellIndex
            : stateIndex - traversalCount;
    };
    const auto stateAnchor = [&](std::size_t stateIndex) {
        return stateIndex < traversalCount
            ? traversals[stateIndex].anchor
            : endpoints.resolvedStart;
    };

    std::vector<float> gScores(stateCount, std::numeric_limits<float>::max());
    std::vector<std::size_t> parents(stateCount, invalidState);
    std::priority_queue<QueueItem> open{};
    for (std::size_t startCellIndex : endpoints.startCells) {
        if (startCellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const std::size_t startState = traversalCount + startCellIndex;
        gScores[startState] = 0.0f;
        open.push(QueueItem{
            glm::distance(endpoints.resolvedStart, endpoints.resolvedDestination),
            0.0f,
            startState
        });
    }
    if (open.empty()) {
        return std::nullopt;
    }

    float bestGoalCost = std::numeric_limits<float>::max();
    std::size_t bestGoalState = invalidState;
    std::size_t expansionCount = 0u;
    while (!open.empty()) {
        if ((expansionCount++ & 31u) == 0u &&
            cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed)) {
            return std::nullopt;
        }

        const QueueItem current = open.top();
        open.pop();
        if (current.fScore >= bestGoalCost - kPolygonEpsilon) {
            break;
        }
        if (current.stateIndex >= stateCount ||
            current.gScore > gScores[current.stateIndex] + kPolygonEpsilon) {
            continue;
        }

        const std::size_t currentCellIndex = stateCell(current.stateIndex);
        if (currentCellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const glm::vec3 currentAnchor = stateAnchor(current.stateIndex);
        if (targetCells[currentCellIndex] != 0u) {
            const float goalCost =
                current.gScore +
                glm::distance(currentAnchor, endpoints.resolvedDestination);
            if (goalCost < bestGoalCost) {
                bestGoalCost = goalCost;
                bestGoalState = current.stateIndex;
            }
        }

        for (std::size_t traversalIndex :
             outgoingTraversals[currentCellIndex]) {
            const NavTraversalState& traversal = traversals[traversalIndex];
            if (!traversal.traversable) {
                continue;
            }
            const NavGraphEdge& edge =
                runtime.graph[currentCellIndex][traversal.edgeIndex];
            if (edge.viaLink &&
                !segmentInsideAuthoredWalkableSurfaceWithClearance(
                    runtime,
                    currentAnchor,
                    edge.linkStartPoint,
                    profile)) {
                continue;
            }
            const float stepCost = edge.viaLink
                ? glm::distance(currentAnchor, edge.linkStartPoint) +
                    glm::distance(edge.linkStartPoint, edge.linkEndPoint)
                : glm::distance(currentAnchor, traversal.anchor);
            const float candidateG = current.gScore + stepCost;
            if (candidateG >= gScores[traversalIndex] - kPolygonEpsilon) {
                continue;
            }
            gScores[traversalIndex] = candidateG;
            parents[traversalIndex] = current.stateIndex;
            open.push(QueueItem{
                candidateG +
                    glm::distance(traversal.anchor, endpoints.resolvedDestination),
                candidateG,
                traversalIndex
            });
        }
    }
    if (bestGoalState == invalidState) {
        return std::nullopt;
    }

    std::vector<NavCorridorStep> reversed{};
    for (std::size_t stateIndex = bestGoalState;
         stateIndex < traversalCount;
         stateIndex = parents[stateIndex]) {
        const NavTraversalState& traversal = traversals[stateIndex];
        reversed.push_back(NavCorridorStep{
            traversal.fromCellIndex,
            traversal.toCellIndex,
            traversal.edgeIndex,
            traversal.safePortal
        });
        if (parents[stateIndex] == invalidState) {
            return std::nullopt;
        }
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

struct OrientedFunnelPortal {
    glm::vec2 left{0.0f};
    glm::vec2 right{0.0f};
};

OrientedFunnelPortal orientPortalForCorridor(
    const NavigationSolveView&,
    std::size_t,
    std::size_t,
    const SharedPortalResult& portal
) {
    return OrientedFunnelPortal{portal.a, portal.b};
}

std::vector<glm::vec2> pullFunnelCorners(
    const glm::vec2& start,
    const std::vector<OrientedFunnelPortal>& corridorPortals,
    const glm::vec2& destination
) {
    std::vector<OrientedFunnelPortal> portals{};
    portals.reserve(corridorPortals.size() + 2u);
    portals.push_back(OrientedFunnelPortal{start, start});
    for (const OrientedFunnelPortal& portal : corridorPortals) {
        if (nearlyEqualVec2(portal.left, portal.right)) {
            continue;
        }
        if (!portals.empty() &&
            nearlyEqualVec2(portals.back().left, portal.left) &&
            nearlyEqualVec2(portals.back().right, portal.right)) {
            continue;
        }
        portals.push_back(portal);
    }
    portals.push_back(OrientedFunnelPortal{destination, destination});

    std::vector<glm::vec2> corners{start};
    glm::vec2 apex = start;
    glm::vec2 left = start;
    glm::vec2 right = start;
    std::size_t apexIndex = 0u;
    std::size_t leftIndex = 0u;
    std::size_t rightIndex = 0u;

    for (std::size_t portalIndex = 1u; portalIndex < portals.size(); ++portalIndex) {
        const glm::vec2 candidateLeft = portals[portalIndex].left;
        const glm::vec2 candidateRight = portals[portalIndex].right;

        if (funnelArea2(apex, right, candidateRight) >=
            -funnelAreaTolerance(apex, right, candidateRight)) {
            if (nearlyEqualVec2(apex, right) ||
                funnelArea2(apex, left, candidateRight) <=
                    funnelAreaTolerance(apex, left, candidateRight)) {
                right = candidateRight;
                rightIndex = portalIndex;
            } else {
                if (!nearlyEqualVec2(corners.back(), left)) {
                    corners.push_back(left);
                }
                apex = left;
                apexIndex = leftIndex;
                left = apex;
                right = apex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                portalIndex = apexIndex;
                continue;
            }
        }

        if (funnelArea2(apex, left, candidateLeft) <=
            funnelAreaTolerance(apex, left, candidateLeft)) {
            if (nearlyEqualVec2(apex, left) ||
                funnelArea2(apex, right, candidateLeft) >=
                    -funnelAreaTolerance(apex, right, candidateLeft)) {
                left = candidateLeft;
                leftIndex = portalIndex;
            } else {
                if (!nearlyEqualVec2(corners.back(), right)) {
                    corners.push_back(right);
                }
                apex = right;
                apexIndex = rightIndex;
                left = apex;
                right = apex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                portalIndex = apexIndex;
            }
        }
    }

    if (!nearlyEqualVec2(corners.back(), destination)) {
        corners.push_back(destination);
    }
    return corners;
}

std::optional<std::vector<glm::vec3>> buildFunnelPath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor
) {
    std::vector<glm::vec3> corners{};
    glm::vec3 segmentStart = endpoints.resolvedStart;
    float segmentElevation = segmentStart.y;
    std::vector<OrientedFunnelPortal> portals{};

    const auto flushFunnel = [&](const glm::vec3& segmentEnd) {
        const std::vector<glm::vec2> pulled = pullFunnelCorners(
            glm::vec2(segmentStart.x, segmentStart.z),
            portals,
            glm::vec2(segmentEnd.x, segmentEnd.z)
        );
        for (std::size_t pointIndex = 1u; pointIndex < pulled.size(); ++pointIndex) {
            const bool isEndpoint = pointIndex + 1u == pulled.size();
            const float elevation = isEndpoint ? segmentEnd.y : segmentElevation;
            appendPathCorner(
                corners,
                glm::vec3(pulled[pointIndex].x, elevation, pulled[pointIndex].y),
                kPolygonEpsilon
            );
        }
    };

    for (const NavCorridorStep& step : corridor) {
        if (step.fromCellIndex >= runtime.graph.size() ||
            step.edgeIndex >= runtime.graph[step.fromCellIndex].size()) {
            return std::nullopt;
        }
        const NavGraphEdge& edge =
            runtime.graph[step.fromCellIndex][step.edgeIndex];
        if (!edge.viaLink) {
            if (!step.safePortal.has_value()) {
                return std::nullopt;
            }
            portals.push_back(orientPortalForCorridor(
                runtime,
                step.fromCellIndex,
                step.toCellIndex,
                *step.safePortal
            ));
            continue;
        }

        flushFunnel(edge.linkStartPoint);
        appendPathCorner(corners, edge.linkEndPoint, kPolygonEpsilon);
        segmentStart = edge.linkEndPoint;
        segmentElevation = edge.linkEndPoint.y;
        portals.clear();
    }

    flushFunnel(endpoints.resolvedDestination);
    return corners;
}

std::optional<std::vector<glm::vec3>> solveCorridorClearancePath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled,
    bool allowVisibilityFallback,
    bool restrictToCorridor = true,
    VisibilityTraversalCache* sharedVisibilityCache = nullptr
) {
    const auto isCancelled = [&]() {
        return cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed);
    };
    if (profile.empty() || isCancelled() || !allowVisibilityFallback) {
        return std::nullopt;
    }

    constexpr std::size_t kMaximumRankedNodes = 512u;
    constexpr std::size_t kInitialLocalVisibilityNodes = 64u;
    constexpr std::size_t kMaximumLocalVisibilityNodes = 72u;
    constexpr std::size_t kMaximumRefinedVisibilityNodes = 96u;
    constexpr float kMaximumUnrefinedStretch = 1.5f;
    std::vector<std::uint8_t> corridorCells(runtime.bakedCells.size(), 0u);
    for (std::size_t cell : endpoints.startCells) {
        if (cell < corridorCells.size()) {
            corridorCells[cell] = 1u;
        }
    }
    for (std::size_t cell : endpoints.targetCells) {
        if (cell < corridorCells.size()) {
            corridorCells[cell] = 1u;
        }
    }
    for (const NavCorridorStep& step : corridor) {
        if (step.fromCellIndex < corridorCells.size()) {
            corridorCells[step.fromCellIndex] = 1u;
        }
        if (step.toCellIndex < corridorCells.size()) {
            corridorCells[step.toCellIndex] = 1u;
        }
    }
    if (!restrictToCorridor) {
        std::fill(corridorCells.begin(), corridorCells.end(), 0u);
        std::vector<std::size_t> pendingCells{};
        for (std::size_t cell : endpoints.startCells) {
            if (cell < corridorCells.size() && corridorCells[cell] == 0u) {
                corridorCells[cell] = 1u;
                pendingCells.push_back(cell);
            }
        }
        while (!pendingCells.empty()) {
            const std::size_t cell = pendingCells.back();
            pendingCells.pop_back();
            for (const NavGraphEdge& edge : runtime.graph[cell]) {
                if (edge.viaLink ||
                    edge.targetCellIndex >= corridorCells.size() ||
                    corridorCells[edge.targetCellIndex] != 0u) {
                    continue;
                }
                corridorCells[edge.targetCellIndex] = 1u;
                pendingCells.push_back(edge.targetCellIndex);
            }
        }
    }
    std::vector<glm::vec3> nodes{
        endpoints.resolvedStart,
        endpoints.resolvedDestination,
    };
    const AgentClearanceProfile nodeClearance =
        headingIndependentNodeClearance(profile);
    const auto nodeCanParticipate = [&](const glm::vec3& point) {
        if (nodeClearance.empty()) {
            return true;
        }
        return visitClearanceBoundaryOffsets(
            nodeClearance,
            glm::vec2(0.0f, 1.0f),
            kSegmentClearanceSampleDirections,
            [&](const glm::vec2& offset) {
                const glm::vec3 sample(
                    point.x + offset.x,
                    point.y,
                    point.z + offset.y
                );
                return segmentInsideBakedWalkableSurface(
                    runtime,
                    sample,
                    sample
                );
            }
        );
    };
    std::unordered_set<QuantizedLayerPoint, QuantizedLayerPointHash> emitted{
        quantizeLayerPoint(
            glm::vec2(endpoints.resolvedStart.x, endpoints.resolvedStart.z),
            endpoints.resolvedStart.y
        ),
        quantizeLayerPoint(
            glm::vec2(
                endpoints.resolvedDestination.x,
                endpoints.resolvedDestination.z
            ),
            endpoints.resolvedDestination.y
        ),
    };
    const auto addNode = [&](const glm::vec3& point) {
        if (isCancelled() ||
            std::abs(point.y - endpoints.resolvedStart.y) >
                kLayerGroupingEpsilon ||
            !segmentInsideBakedWalkableSurface(
                runtime,
                point,
                point)) {
            return;
        }
        const QuantizedLayerPoint key =
            quantizeLayerPoint(glm::vec2(point.x, point.z), point.y);
        if (emitted.insert(key).second) {
            nodes.push_back(point);
        }
    };

    for (const NavCorridorStep& step : corridor) {
        if (isCancelled()) {
            return std::nullopt;
        }
        if (!step.safePortal.has_value()) {
            continue;
        }
        const glm::vec2 midpoint =
            (step.safePortal->a + step.safePortal->b) * 0.5f;
        const float elevation =
            step.fromCellIndex < runtime.bakedCells.size()
            ? runtime.bakedCells[step.fromCellIndex].elevationY
            : endpoints.resolvedStart.y;
        addNode(glm::vec3(midpoint.x, elevation, midpoint.y));
        addNode(glm::vec3(
            step.safePortal->a.x,
            elevation,
            step.safePortal->a.y
        ));
        addNode(glm::vec3(
            step.safePortal->b.x,
            elevation,
            step.safePortal->b.y
        ));
    }

    const float clearanceRadius =
        conservativeClearanceRadius(profile) * 1.05f;
    constexpr int kCornerCandidateDirections = 8;
    for (std::size_t cellIndex = 0u;
         cellIndex < runtime.bakedCells.size();
         ++cellIndex) {
        if (isCancelled()) {
            return std::nullopt;
        }
        if (corridorCells[cellIndex] == 0u) {
            continue;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        for (std::size_t vertexIndex = 0u;
             vertexIndex < cell.verticesXZ.size();
             ++vertexIndex) {
            if ((vertexIndex & 7u) == 0u && isCancelled()) {
                return std::nullopt;
            }
            if (cellIndex < runtime.bakedCellBoundaryVertices.size() &&
                vertexIndex <
                    runtime.bakedCellBoundaryVertices[cellIndex].size() &&
                runtime.bakedCellBoundaryVertices[cellIndex][vertexIndex] == 0u) {
                continue;
            }
            const glm::vec2 vertex = cell.verticesXZ[vertexIndex];
            for (int directionIndex = 0;
                 directionIndex < kCornerCandidateDirections;
                 ++directionIndex) {
                const glm::vec2 direction = clearanceSampleDirection(
                    directionIndex,
                    kCornerCandidateDirections
                );
                addNode(glm::vec3(
                    vertex.x + direction.x * clearanceRadius,
                    cell.elevationY,
                    vertex.y + direction.y * clearanceRadius
                ));
            }
        }
    }

    if (isCancelled()) {
        return std::nullopt;
    }
    if (nodes.size() > 2u) {
        std::stable_sort(
            nodes.begin() + 2,
            nodes.end(),
            [&](const glm::vec3& lhs, const glm::vec3& rhs) {
                const float lhsScore =
                    glm::distance(endpoints.resolvedStart, lhs) +
                    glm::distance(lhs, endpoints.resolvedDestination);
                const float rhsScore =
                    glm::distance(endpoints.resolvedStart, rhs) +
                    glm::distance(rhs, endpoints.resolvedDestination);
                return lhsScore < rhsScore;
            }
        );
        // Apply the collection budget only after global ranking. Capping while
        // iterating cells made the result depend on cell index and could keep
        // a 500-unit portal midpoint while dropping nearby obstacle-corner
        // offsets owned by a later cell.
        if (nodes.size() > kMaximumRankedNodes) {
            nodes.resize(kMaximumRankedNodes);
        }
    }

    // Candidate generation can emit hundreds of corner offsets. Validate
    // clearance only after ranking them, and keep scanning until the bounded
    // visibility graph is full. Previously every collected point paid for an
    // exact footprint/union test before most of them were discarded.
    std::vector<glm::vec3> rankedNodes = std::move(nodes);
    nodes.clear();
    nodes.reserve(std::min(
        rankedNodes.size(),
        kMaximumLocalVisibilityNodes
    ));
    nodes.push_back(rankedNodes[0]);
    nodes.push_back(rankedNodes[1]);
    std::size_t nextRankedNodeIndex = 2u;
    const auto appendRankedNodes = [&](std::size_t limit) {
        while (nextRankedNodeIndex < rankedNodes.size() &&
               nodes.size() < limit) {
            if (isCancelled()) {
                return false;
            }
            // Segment validation below performs the exact swept-footprint
            // test for every accepted edge. Node admission only needs a
            // conservative boundary prefilter; applying a circumscribed
            // capsule to an isolated node rejected valid tangent junctions
            // before any edge was tested.
            const glm::vec3& candidate =
                rankedNodes[nextRankedNodeIndex++];
            if (nodeCanParticipate(candidate)) {
                nodes.push_back(candidate);
            }
        }
        return true;
    };
    if (!appendRankedNodes(kInitialLocalVisibilityNodes)) {
        return std::nullopt;
    }

    struct QueueItem {
        float fScore{0.0f};
        float gScore{0.0f};
        std::size_t nodeIndex{0u};

        bool operator<(const QueueItem& other) const {
            return fScore > other.fScore;
        }
    };
    std::unordered_map<std::uint64_t, bool> visibilityCache{};
    visibilityCache.reserve(kMaximumRefinedVisibilityNodes * 32u);
    const auto canTraverse = [&](std::size_t lhs, std::size_t rhs) {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(lhs) << 32u) |
            static_cast<std::uint64_t>(rhs);
        if (const auto found = visibilityCache.find(key);
            found != visibilityCache.end()) {
            return found->second;
        }
        const VisibilitySegmentKey sharedKey{
            exactLayerPoint(nodes[lhs]),
            exactLayerPoint(nodes[rhs]),
        };
        // Every corridor in this query uses the same immutable runtime and
        // clearance profile. Keep the key directed (box offsets can make a
        // sweep direction-sensitive) and use exact float bits so caching can
        // never merge two merely-near tangent segments.
        if (sharedVisibilityCache != nullptr) {
            if (const auto shared = sharedVisibilityCache->find(sharedKey);
                shared != sharedVisibilityCache->end()) {
                visibilityCache.emplace(key, shared->second);
                return shared->second;
            }
        }
        const bool visible =
            segmentInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                nodes[lhs],
                nodes[rhs],
                profile
            );
        if (sharedVisibilityCache != nullptr) {
            sharedVisibilityCache->emplace(sharedKey, visible);
        }
        visibilityCache.emplace(key, visible);
        return visible;
    };

    struct VisibilitySearchResult {
        float length{std::numeric_limits<float>::max()};
        std::vector<std::size_t> path{};
    };
    const auto searchVisibilityGraph = [&]()
        -> std::optional<VisibilitySearchResult> {
        const std::size_t nodeCount = nodes.size();
        std::vector<float> distances(
            nodeCount,
            std::numeric_limits<float>::max());
        std::vector<std::size_t> parents(
            nodeCount,
            std::numeric_limits<std::size_t>::max());
        std::vector<std::uint8_t> closed(nodeCount, 0u);
        std::priority_queue<QueueItem> open{};
        distances[0] = 0.0f;
        open.push(QueueItem{
            glm::distance(nodes[0], nodes[1]),
            0.0f,
            0u,
        });
        std::optional<VisibilitySearchResult> best{};
        std::size_t expansionCount = 0u;
        while (!open.empty()) {
            if ((expansionCount++ & 15u) == 0u && isCancelled()) {
                return std::nullopt;
            }
            const QueueItem current = open.top();
            open.pop();
            if (best.has_value() &&
                current.fScore >= best->length - kPolygonEpsilon) {
                break;
            }
            if (current.gScore >
                    distances[current.nodeIndex] + kPolygonEpsilon ||
                closed[current.nodeIndex] != 0u) {
                continue;
            }
            closed[current.nodeIndex] = 1u;

            const auto considerNode = [&](std::size_t nextNode) {
                if (nextNode == 0u || nextNode == current.nodeIndex ||
                    nextNode >= nodeCount || closed[nextNode] != 0u) {
                    return;
                }
                const float candidate =
                    distances[current.nodeIndex] +
                    glm::distance(nodes[current.nodeIndex], nodes[nextNode]);
                const float estimate = candidate +
                    (nextNode == 1u
                        ? 0.0f
                        : glm::distance(nodes[nextNode], nodes[1]));
                if ((best.has_value() &&
                     estimate >= best->length - kPolygonEpsilon) ||
                    (nextNode != 1u &&
                     candidate >= distances[nextNode] - kPolygonEpsilon) ||
                    !canTraverse(current.nodeIndex, nextNode)) {
                    return;
                }
                if (nextNode == 1u) {
                    VisibilitySearchResult result{};
                    result.length = candidate;
                    result.path.push_back(1u);
                    for (std::size_t pathNode = current.nodeIndex;;) {
                        result.path.push_back(pathNode);
                        if (pathNode == 0u) {
                            break;
                        }
                        pathNode = parents[pathNode];
                        if (pathNode ==
                            std::numeric_limits<std::size_t>::max()) {
                            return;
                        }
                    }
                    std::reverse(result.path.begin(), result.path.end());
                    best = std::move(result);
                    return;
                }
                distances[nextNode] = candidate;
                parents[nextNode] = current.nodeIndex;
                open.push(QueueItem{estimate, candidate, nextNode});
            };

            for (std::size_t nextNode = 1u;
                 nextNode < nodeCount;
                 ++nextNode) {
                considerNode(nextNode);
            }
        }
        return best;
    };

    std::optional<VisibilitySearchResult> visibilityResult =
        searchVisibilityGraph();
    if (!visibilityResult.has_value() &&
        nodes.size() < kMaximumLocalVisibilityNodes &&
        nextRankedNodeIndex < rankedNodes.size()) {
        if (!appendRankedNodes(kMaximumLocalVisibilityNodes)) {
            return std::nullopt;
        }
        visibilityResult = searchVisibilityGraph();
    }
    if (profile.shape == AgentClearanceShape::Box &&
        restrictToCorridor &&
        !corridor.empty()) {
        // The cheap radial corner samples normally suffice. A very long
        // result relative to the raw funnel is a strong signal that a
        // heading-aligned box tangent was missed (notably on long portals,
        // whose midpoint can be hundreds of units away). In that case only,
        // add analytic footprint tangents at the constraining funnel corners.
        std::vector<NavCorridorStep> rawPortalCorridor = corridor;
        for (NavCorridorStep& step : rawPortalCorridor) {
            if (step.fromCellIndex >= runtime.graph.size() ||
                step.edgeIndex >= runtime.graph[step.fromCellIndex].size()) {
                continue;
            }
            const NavGraphEdge& edge =
                runtime.graph[step.fromCellIndex][step.edgeIndex];
            if (!edge.viaLink) {
                step.safePortal = SharedPortalResult{
                    edge.portalA,
                    edge.portalB,
                };
            }
        }
        const std::optional<std::vector<glm::vec3>> rawGuide =
            buildFunnelPath(runtime, endpoints, rawPortalCorridor);
        const float rawGuideLength = rawGuide.has_value()
            ? pathLength(endpoints.resolvedStart, *rawGuide)
            : std::numeric_limits<float>::max();
        const bool needsTangentRefinement =
            rawGuide.has_value() &&
            (!visibilityResult.has_value() ||
             visibilityResult->length >
                rawGuideLength * kMaximumUnrefinedStretch);
        if (needsTangentRefinement) {
            std::vector<glm::vec3> tangentCandidates{};
            tangentCandidates.reserve(rawGuide->size() * 8u);
            const auto addTangentsForReference = [&](
                const glm::vec3& vertex,
                const glm::vec3& reference,
                bool movingTowardReference
            ) {
                const glm::vec2 vertexXZ(vertex.x, vertex.z);
                const glm::vec2 referenceXZ(reference.x, reference.z);
                for (float lateralSign : {-1.0f, 1.0f}) {
                    for (float longitudinalSign : {-1.0f, 1.0f}) {
                        glm::vec2 forward = normalizeOrFallback(
                            movingTowardReference
                                ? referenceXZ - vertexXZ
                                : vertexXZ - referenceXZ
                        );
                        glm::vec2 origin = vertexXZ;
                        for (int refinement = 0;
                             refinement < 3;
                             ++refinement) {
                            const glm::vec2 right(forward.y, -forward.x);
                            const glm::vec2 center =
                                rotateLocalXZToPlanar(
                                    profile.centerXZ,
                                    forward
                                );
                            const glm::vec2 footprintCorner = center +
                                right * profile.boxHalfExtentsXZ.x *
                                    lateralSign +
                                forward * profile.boxHalfExtentsXZ.y *
                                    longitudinalSign;
                            origin = vertexXZ - footprintCorner;
                            forward = normalizeOrFallback(
                                movingTowardReference
                                    ? referenceXZ - origin
                                    : origin - referenceXZ,
                                forward
                            );
                        }
                        tangentCandidates.emplace_back(
                            origin.x,
                            vertex.y,
                            origin.y
                        );
                    }
                }
            };
            for (const glm::vec3& guideCorner : *rawGuide) {
                if (nearlyEqualVec3(
                        guideCorner,
                        endpoints.resolvedDestination,
                        kPolygonEpsilon)) {
                    continue;
                }
                addTangentsForReference(
                    guideCorner,
                    endpoints.resolvedStart,
                    false
                );
                addTangentsForReference(
                    guideCorner,
                    endpoints.resolvedDestination,
                    true
                );
            }
            std::stable_sort(
                tangentCandidates.begin(),
                tangentCandidates.end(),
                [&](const glm::vec3& lhs, const glm::vec3& rhs) {
                    const float lhsScore =
                        glm::distance(endpoints.resolvedStart, lhs) +
                        glm::distance(lhs, endpoints.resolvedDestination);
                    const float rhsScore =
                        glm::distance(endpoints.resolvedStart, rhs) +
                        glm::distance(rhs, endpoints.resolvedDestination);
                    return lhsScore < rhsScore;
                }
            );
            std::unordered_set<
                QuantizedLayerPoint,
                QuantizedLayerPointHash
            > selectedNodes{};
            selectedNodes.reserve(nodes.size() + tangentCandidates.size());
            for (const glm::vec3& node : nodes) {
                selectedNodes.insert(quantizeLayerPoint(
                    glm::vec2(node.x, node.z),
                    node.y
                ));
            }
            for (const glm::vec3& candidate : tangentCandidates) {
                if (nodes.size() >= kMaximumRefinedVisibilityNodes ||
                    isCancelled()) {
                    break;
                }
                const QuantizedLayerPoint key = quantizeLayerPoint(
                    glm::vec2(candidate.x, candidate.z),
                    candidate.y
                );
                if (selectedNodes.insert(key).second &&
                    nodeCanParticipate(candidate)) {
                    nodes.push_back(candidate);
                }
            }
            if (isCancelled()) {
                return std::nullopt;
            }
            const std::optional<VisibilitySearchResult> refinedResult =
                searchVisibilityGraph();
            if (refinedResult.has_value() &&
                (!visibilityResult.has_value() ||
                 refinedResult->length < visibilityResult->length)) {
                visibilityResult = refinedResult;
            }
        }
    }
    if (!visibilityResult.has_value() || visibilityResult->path.size() < 2u) {
        return std::nullopt;
    }
    std::vector<glm::vec3> result{};
    result.reserve(visibilityResult->path.size() - 1u);
    for (std::size_t pathIndex = 1u;
         pathIndex < visibilityResult->path.size();
         ++pathIndex) {
        result.push_back(nodes[visibilityResult->path[pathIndex]]);
    }
    return result;
}

bool segmentMatchesRuntimeLink(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    float epsilon
) {
    for (const std::vector<NavGraphEdge>& edges : runtime.graph) {
        for (const NavGraphEdge& edge : edges) {
            if (!edge.viaLink) {
                continue;
            }
            if (nearlyEqualVec3(from, edge.linkStartPoint, epsilon) &&
                nearlyEqualVec3(to, edge.linkEndPoint, epsilon)) {
                return true;
            }
        }
    }
    return false;
}

bool pathSegmentsAreValid(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const std::vector<glm::vec3>& corners,
    const AgentClearanceProfile& profile
) {
    glm::vec3 previous = start;
    for (const glm::vec3& corner : corners) {
        if (!segmentInsideAuthoredWalkableSurfaceWithClearance(runtime, previous, corner, profile) &&
            !segmentMatchesRuntimeLink(runtime, previous, corner, kPolygonEpsilon * 8.0f)) {
            return false;
        }
        previous = corner;
    }
    return true;
}

std::optional<std::vector<glm::vec3>> solveLinkedCorridorClearancePath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled
) {
    const auto isCancelled = [&]() {
        return cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed);
    };
    if (profile.empty() || isCancelled()) {
        return std::nullopt;
    }

    std::vector<glm::vec3> result{};
    std::vector<NavCorridorStep> planarCorridor{};
    ResolvedPathEndpoints planarEndpoints{};
    planarEndpoints.resolvedStart = endpoints.resolvedStart;
    planarEndpoints.startCells = endpoints.startCells;

    const auto flushPlanar = [&](
        const glm::vec3& segmentDestination,
        const std::vector<std::size_t>& targetCells
    ) -> bool {
        if (isCancelled()) {
            return false;
        }
        planarEndpoints.resolvedDestination = segmentDestination;
        planarEndpoints.targetCells = targetCells;

        std::optional<std::vector<glm::vec3>> segmentCorners =
            buildFunnelPath(runtime, planarEndpoints, planarCorridor);
        if (!segmentCorners.has_value() ||
            !pathSegmentsAreValid(
                runtime,
                planarEndpoints.resolvedStart,
                *segmentCorners,
                profile)) {
            segmentCorners = solveCorridorClearancePath(
                runtime,
                planarEndpoints,
                planarCorridor,
                profile,
                cancelled,
                true
            );
        }
        if (!segmentCorners.has_value() ||
            !pathSegmentsAreValid(
                runtime,
                planarEndpoints.resolvedStart,
                *segmentCorners,
                profile)) {
            return false;
        }
        for (const glm::vec3& corner : *segmentCorners) {
            appendPathCorner(result, corner, kPolygonEpsilon);
        }
        planarCorridor.clear();
        planarEndpoints.resolvedStart = segmentDestination;
        planarEndpoints.startCells = targetCells;
        return true;
    };

    for (const NavCorridorStep& step : corridor) {
        if (isCancelled() || step.fromCellIndex >= runtime.graph.size() ||
            step.edgeIndex >= runtime.graph[step.fromCellIndex].size()) {
            return std::nullopt;
        }
        const NavGraphEdge& edge =
            runtime.graph[step.fromCellIndex][step.edgeIndex];
        if (!edge.viaLink) {
            planarCorridor.push_back(step);
            continue;
        }

        const std::vector<std::size_t> linkStartCells{step.fromCellIndex};
        if (!flushPlanar(edge.linkStartPoint, linkStartCells)) {
            return std::nullopt;
        }
        appendPathCorner(result, edge.linkEndPoint, kPolygonEpsilon);
        planarEndpoints.resolvedStart = edge.linkEndPoint;
        planarEndpoints.startCells = {step.toCellIndex};
    }

    if (!flushPlanar(
            endpoints.resolvedDestination,
            endpoints.targetCells)) {
        return std::nullopt;
    }
    if (!pathSegmentsAreValid(
            runtime,
            endpoints.resolvedStart,
            result,
            profile)) {
        return std::nullopt;
    }
    return result;
}

std::optional<SolvedPath> solvePathCorners(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const glm::vec3& destination,
    float arrivalRadius,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled = nullptr
) {
    const auto isCancelled = [&]() {
        return cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed);
    };
    const std::optional<ResolvedPathEndpoints> endpoints =
        resolvePathEndpoints(
            runtime,
            start,
            destination,
            profile,
            cancelled
        );
    if (!endpoints.has_value() || isCancelled()) {
        return std::nullopt;
    }

    if (canSolveDirectPath(runtime, *endpoints, profile)) {
        std::vector<glm::vec3> directCorners{};
        appendPathCorner(directCorners, endpoints->resolvedDestination, arrivalRadius);
        return SolvedPath{
            endpoints->resolvedDestination,
            std::move(directCorners),
            endpoints->resolvedStart
        };
    }

    // Polyanya searches continuous intervals instead of a handful of sampled
    // points on each portal. On a conforming convex mesh it returns the global
    // Euclidean shortest path directly; applying a funnel afterwards would be
    // redundant. Off-mesh links and collider clearance remain on the legacy
    // validated pipeline until they have an exact configuration-space mesh.
    std::optional<std::vector<glm::vec3>> exactPlanarCorners{};
    if (profile.empty() && runtime.polyanyaMesh != nullptr) {
        if (std::optional<navigation_detail::PolyanyaPath> exactPath =
                navigation_detail::findPolyanyaPath(
                    *runtime.polyanyaMesh,
                    endpoints->resolvedStart,
                    endpoints->resolvedDestination,
                    endpoints->startCells,
                    endpoints->targetCells,
                    cancelled);
            exactPath.has_value() &&
            pathSegmentsAreValid(
                runtime,
                endpoints->resolvedStart,
                exactPath->corners,
                profile)) {
            if (exactPath->corners.size() == 1u &&
                glm::distance(
                    endpoints->resolvedStart,
                    exactPath->corners.front()) <= arrivalRadius) {
                exactPath->corners.clear();
            }

            if (runtime.asset.links.empty()) {
                return SolvedPath{
                    endpoints->resolvedDestination,
                    std::move(exactPath->corners),
                    endpoints->resolvedStart
                };
            }

            // A NavLink only matters when it lies on some directed graph path
            // from this query's start set to its target set.  Unresolved links,
            // links in another component and dead-end links must not demote an
            // otherwise exact Polyanya query to the sampled fallback.
            std::vector<std::uint8_t> reachableFromStart(
                runtime.graph.size(),
                0u);
            std::vector<std::size_t> pending{};
            for (std::size_t cell : endpoints->startCells) {
                if (cell < reachableFromStart.size() &&
                    reachableFromStart[cell] == 0u) {
                    reachableFromStart[cell] = 1u;
                    pending.push_back(cell);
                }
            }
            while (!pending.empty()) {
                const std::size_t cell = pending.back();
                pending.pop_back();
                for (const NavGraphEdge& edge : runtime.graph[cell]) {
                    if (edge.targetCellIndex < reachableFromStart.size() &&
                        reachableFromStart[edge.targetCellIndex] == 0u) {
                        reachableFromStart[edge.targetCellIndex] = 1u;
                        pending.push_back(edge.targetCellIndex);
                    }
                }
            }

            std::vector<std::vector<std::size_t>> reverseGraph(
                runtime.graph.size());
            for (std::size_t cell = 0u; cell < runtime.graph.size(); ++cell) {
                for (const NavGraphEdge& edge : runtime.graph[cell]) {
                    if (edge.targetCellIndex < reverseGraph.size()) {
                        reverseGraph[edge.targetCellIndex].push_back(cell);
                    }
                }
            }
            std::vector<std::uint8_t> canReachTarget(runtime.graph.size(), 0u);
            for (std::size_t cell : endpoints->targetCells) {
                if (cell < canReachTarget.size() && canReachTarget[cell] == 0u) {
                    canReachTarget[cell] = 1u;
                    pending.push_back(cell);
                }
            }
            while (!pending.empty()) {
                const std::size_t cell = pending.back();
                pending.pop_back();
                for (std::size_t predecessor : reverseGraph[cell]) {
                    if (canReachTarget[predecessor] == 0u) {
                        canReachTarget[predecessor] = 1u;
                        pending.push_back(predecessor);
                    }
                }
            }

            bool hasRelevantLink = false;
            for (std::size_t cell = 0u;
                 cell < runtime.graph.size() && !hasRelevantLink;
                 ++cell) {
                if (reachableFromStart[cell] == 0u) {
                    continue;
                }
                hasRelevantLink = std::any_of(
                    runtime.graph[cell].begin(),
                    runtime.graph[cell].end(),
                    [&](const NavGraphEdge& edge) {
                        return edge.viaLink &&
                            edge.targetCellIndex < canReachTarget.size() &&
                            canReachTarget[edge.targetCellIndex] != 0u;
                    }
                );
            }
            if (!hasRelevantLink) {
                return SolvedPath{
                    endpoints->resolvedDestination,
                    std::move(exactPath->corners),
                    endpoints->resolvedStart
                };
            }
            // Keep the exact no-link route as a baseline.  The link-aware A*
            // result below is accepted only when its validated final geometry
            // is actually shorter.
            exactPlanarCorners = std::move(exactPath->corners);
        }
    }

    std::vector<std::vector<NavCorridorStep>> corridorCandidates{};
    constexpr std::size_t kMaximumCorridorSearches = 14u;
    std::size_t corridorSearchCount = 0u;
    const auto appendCorridorCandidate = [&](
        PortalSamplingMode samplingMode,
        const std::vector<std::pair<std::size_t, std::size_t>>&
            blockedTraversals,
        const AgentClearanceProfile& corridorProfile
    ) {
        if (corridorSearchCount >= kMaximumCorridorSearches ||
            isCancelled()) {
            return;
        }
        ++corridorSearchCount;
        std::optional<std::vector<NavCorridorStep>> candidate =
            findAStarCorridor(
                runtime,
                *endpoints,
                corridorProfile,
                cancelled,
                samplingMode,
                blockedTraversals
            );
        if (!candidate.has_value()) {
            return;
        }
        const bool duplicate = std::any_of(
            corridorCandidates.begin(),
            corridorCandidates.end(),
            [&](const std::vector<NavCorridorStep>& existing) {
                return existing.size() == candidate->size() &&
                    std::equal(
                        existing.begin(),
                        existing.end(),
                        candidate->begin(),
                        [](const NavCorridorStep& lhs,
                           const NavCorridorStep& rhs) {
                            return lhs.fromCellIndex ==
                                    rhs.fromCellIndex &&
                                lhs.toCellIndex ==
                                    rhs.toCellIndex &&
                                lhs.edgeIndex == rhs.edgeIndex;
                        }
                    );
            }
        );
        if (!duplicate) {
            corridorCandidates.push_back(std::move(*candidate));
        }
    };

    constexpr std::array<PortalSamplingMode, 2u> kSamplingModes{
        PortalSamplingMode::Midpoint,
        PortalSamplingMode::Geometry,
    };
    for (PortalSamplingMode samplingMode : kSamplingModes) {
        appendCorridorCandidate(samplingMode, {}, profile);
    }

    // Bounded k-alternative search: force deviations at evenly distributed
    // portals of the best corridors.  Funnel length, not the A* representative
    // polyline, decides between the resulting corridors below.
    std::vector<std::uint8_t> endpointCells(runtime.graph.size(), 0u);
    for (std::size_t cellIndex : endpoints->startCells) {
        if (cellIndex < endpointCells.size()) {
            endpointCells[cellIndex] = 1u;
        }
    }
    for (std::size_t cellIndex : endpoints->targetCells) {
        if (cellIndex < endpointCells.size()) {
            endpointCells[cellIndex] = 1u;
        }
    }
    bool graphHasBranches = false;
    for (std::size_t cellIndex = 0u;
         cellIndex < runtime.graph.size();
         ++cellIndex) {
        // Interior degree two is just a chain, while degree two at a query
        // endpoint is already a real choice between two homotopies.
        const std::size_t branchDegree =
            endpointCells[cellIndex] != 0u ? 1u : 2u;
        if (runtime.graph[cellIndex].size() > branchDegree) {
            graphHasBranches = true;
            break;
        }
    }
    const bool shouldExploreAlternatives =
        corridorCandidates.size() > 1u || graphHasBranches;
    std::size_t sourceCandidateIndex = 0u;
    while (shouldExploreAlternatives &&
           sourceCandidateIndex < corridorCandidates.size() &&
           corridorSearchCount < kMaximumCorridorSearches) {
        const std::vector<NavCorridorStep> source =
            corridorCandidates[sourceCandidateIndex++];
        if (source.empty()) {
            continue;
        }
        const std::size_t deviationCount =
            std::min<std::size_t>(4u, source.size());
        std::vector<std::size_t> deviationIndices{};
        deviationIndices.reserve(deviationCount);
        for (std::size_t deviation = 0u;
             deviation < deviationCount;
             ++deviation) {
            const std::size_t stepIndex = deviationCount == 1u
                ? 0u
                : deviation * (source.size() - 1u) /
                    (deviationCount - 1u);
            if (deviationIndices.empty() ||
                deviationIndices.back() != stepIndex) {
                deviationIndices.push_back(stepIndex);
            }
        }
        for (std::size_t stepIndex : deviationIndices) {
            const NavCorridorStep& blockedStep = source[stepIndex];
            const std::vector<std::pair<std::size_t, std::size_t>>
                blockedTraversals{
                    {
                        blockedStep.fromCellIndex,
                        blockedStep.edgeIndex,
                    },
                };
            for (PortalSamplingMode samplingMode : kSamplingModes) {
                appendCorridorCandidate(
                    samplingMode,
                    blockedTraversals,
                    profile
                );
            }
            if (corridorSearchCount >= kMaximumCorridorSearches) {
                break;
            }
        }
    }
    if (corridorCandidates.empty() || isCancelled()) {
        if (exactPlanarCorners.has_value() && !isCancelled()) {
            return SolvedPath{
                endpoints->resolvedDestination,
                std::move(*exactPlanarCorners),
                endpoints->resolvedStart
            };
        }
        return std::nullopt;
    }

    struct CorridorPathCandidate {
        const std::vector<NavCorridorStep>* corridor{nullptr};
        std::vector<glm::vec3> rawCorners{};
        float rawLength{std::numeric_limits<float>::max()};
        bool rawIsValid{false};
    };
    std::vector<CorridorPathCandidate> pathCandidates{};
    pathCandidates.reserve(corridorCandidates.size());
    for (std::size_t corridorIndex = 0u;
         corridorIndex < corridorCandidates.size();
         ++corridorIndex) {
        const std::vector<NavCorridorStep>& corridorCandidate =
            corridorCandidates[corridorIndex];
        std::optional<std::vector<glm::vec3>> candidateCorners =
            buildFunnelPath(runtime, *endpoints, corridorCandidate);
        if (!candidateCorners.has_value() || candidateCorners->empty()) {
            continue;
        }
        const float rawLength = pathLength(
            endpoints->resolvedStart,
            *candidateCorners
        );
        pathCandidates.push_back(CorridorPathCandidate{
            &corridorCandidate,
            std::move(*candidateCorners),
            rawLength,
            false,
        });
        // Evaluate after the move; using the stored vector keeps this aggregate
        // construction simple and avoids copying every raw funnel path.
        pathCandidates.back().rawIsValid = pathSegmentsAreValid(
            runtime,
            endpoints->resolvedStart,
            pathCandidates.back().rawCorners,
            profile
        );
    }
    std::sort(
        pathCandidates.begin(),
        pathCandidates.end(),
        [](const CorridorPathCandidate& lhs,
           const CorridorPathCandidate& rhs) {
            return lhs.rawLength < rhs.rawLength;
        }
    );

    std::optional<std::vector<glm::vec3>> corners{};
    float selectedLength = std::numeric_limits<float>::max();
    VisibilityTraversalCache sharedVisibilityCache{};
    sharedVisibilityCache.reserve(4096u);
    for (CorridorPathCandidate& candidate : pathCandidates) {
        if (isCancelled()) {
            return std::nullopt;
        }
        bool pathIsValid = candidate.rawIsValid;
        std::vector<glm::vec3> candidateCorners =
            std::move(candidate.rawCorners);
        const bool corridorUsesLink = std::any_of(
            candidate.corridor->begin(),
            candidate.corridor->end(),
            [&](const NavCorridorStep& step) {
                return step.fromCellIndex < runtime.graph.size() &&
                    step.edgeIndex < runtime.graph[step.fromCellIndex].size() &&
                    runtime.graph[step.fromCellIndex][step.edgeIndex].viaLink;
            }
        );
        if (!pathIsValid && !profile.empty()) {
            if (corridorUsesLink) {
                if (std::optional<std::vector<glm::vec3>> linkedPath =
                        solveLinkedCorridorClearancePath(
                            runtime,
                            *endpoints,
                            *candidate.corridor,
                            profile,
                            cancelled);
                    linkedPath.has_value()) {
                    candidateCorners = std::move(*linkedPath);
                    pathIsValid = true;
                }
            } else {
                // Each corridor contributes only its own portals and boundary
                // nodes. Traversal itself is validated against the complete
                // authored surface, so identical directed segment checks are
                // safely reusable by the other corridor alternatives.
                if (std::optional<std::vector<glm::vec3>> clearancePath =
                        solveCorridorClearancePath(
                            runtime,
                            *endpoints,
                            *candidate.corridor,
                            profile,
                            cancelled,
                            true,
                            true,
                            &sharedVisibilityCache);
                    clearancePath.has_value() &&
                    pathSegmentsAreValid(
                        runtime,
                        endpoints->resolvedStart,
                        *clearancePath,
                        profile)) {
                    candidateCorners = std::move(*clearancePath);
                    pathIsValid = true;
                }
            }
        }
        if (!pathIsValid) {
            continue;
        }
        const float finalLength = pathLength(
            endpoints->resolvedStart,
            candidateCorners
        );
        if (finalLength < selectedLength) {
            selectedLength = finalLength;
            corners = std::move(candidateCorners);
        }
    }
    if (!profile.empty() && !corners.has_value()) {
        // If every corridor-local repair failed, retry once over the complete
        // planar component. This is a completeness fallback, not a path-length
        // heuristic; successful local solutions are compared above by their
        // validated final geometry.
        if (std::optional<std::vector<glm::vec3>> globalPath =
                solveCorridorClearancePath(
                    runtime,
                    *endpoints,
                    {},
                    profile,
                    cancelled,
                    true,
                    false,
                    &sharedVisibilityCache);
            globalPath.has_value() &&
            pathSegmentsAreValid(
                runtime,
                endpoints->resolvedStart,
                *globalPath,
                profile)) {
            const float globalLength = pathLength(
                endpoints->resolvedStart,
                *globalPath
            );
            if (globalLength < selectedLength) {
                selectedLength = globalLength;
                corners = std::move(*globalPath);
            }
        }
    }
    if (isCancelled()) {
        return std::nullopt;
    }
    if (exactPlanarCorners.has_value()) {
        const float exactPlanarLength = pathLength(
            endpoints->resolvedStart,
            *exactPlanarCorners
        );
        if (!corners.has_value() ||
            exactPlanarLength <= selectedLength + kPolygonEpsilon) {
            selectedLength = exactPlanarLength;
            corners = std::move(*exactPlanarCorners);
        }
    }
    if (!corners.has_value()) {
        return std::nullopt;
    }
    if (corners->size() == 1u &&
        glm::distance(endpoints->resolvedStart, corners->front()) <= arrivalRadius) {
        corners->clear();
    }
    return SolvedPath{
        endpoints->resolvedDestination,
        std::move(*corners),
        endpoints->resolvedStart
    };
}

std::vector<glm::vec3> trimPathCornersFromCurrentPosition(
    const NavigationSolveView& runtime,
    const glm::vec3& currentPosition,
    std::vector<glm::vec3> corners,
    float arrivalRadius,
    const AgentClearanceProfile& profile
) {
    while (!corners.empty() && glm::distance(currentPosition, corners.front()) <= arrivalRadius) {
        corners.erase(corners.begin());
    }
    for (std::size_t candidateIndex = corners.size(); candidateIndex-- > 0u;) {
        if (segmentInsideAuthoredWalkableSurfaceWithClearance(runtime, currentPosition, corners[candidateIndex], profile)) {
            corners.erase(corners.begin(), corners.begin() + static_cast<std::ptrdiff_t>(candidateIndex));
            break;
        }
    }
    return corners;
}

void appendGraphEdgeIfMissing(
    std::vector<NavGraphEdge>& edges,
    const NavGraphEdge& candidate
) {
    const auto it = std::find_if(edges.begin(), edges.end(), [&](const NavGraphEdge& existing) {
        return existing.targetCellIndex == candidate.targetCellIndex &&
            existing.viaLink == candidate.viaLink &&
            existing.linkId == candidate.linkId &&
            nearlyEqualVec2(existing.portalA, candidate.portalA) &&
            nearlyEqualVec2(existing.portalB, candidate.portalB) &&
            nearlyEqualVec3(existing.linkStartPoint, candidate.linkStartPoint) &&
            nearlyEqualVec3(existing.linkEndPoint, candidate.linkEndPoint);
    });
    if (it == edges.end()) {
        edges.push_back(candidate);
    }
}

void rebuildCellBoundaryVertexCache(NavigationRuntime& runtime) {
    runtime.bakedCellBoundaryVertices.clear();
    runtime.bakedCellBoundaryVertices.resize(runtime.bakedCells.size());
    std::unordered_set<QuantizedLayerPoint, QuantizedLayerPointHash> boundaryPoints{};

    for (std::size_t cellIndex = 0u; cellIndex < runtime.bakedCells.size(); ++cellIndex) {
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        runtime.bakedCellBoundaryVertices[cellIndex].assign(cell.verticesXZ.size(), 0u);
        if (cell.verticesXZ.size() < 3u || cellIndex >= runtime.graph.size()) {
            continue;
        }

        const auto edgeCoveredFromVertex = [&](const glm::vec2& vertex, const glm::vec2& adjacent) {
            const glm::vec2 edgeDelta = adjacent - vertex;
            const float edgeLength = glm::length(edgeDelta);
            if (edgeLength <= kPlaneEpsilon) {
                return false;
            }
            const float probeDistance = std::min(edgeLength * 0.25f, kPolygonEpsilon * 8.0f);
            const glm::vec2 probe = vertex + edgeDelta * (probeDistance / edgeLength);
            for (const NavGraphEdge& graphEdge : runtime.graph[cellIndex]) {
                if (!graphEdge.viaLink && pointOnSegmentXZ(probe, graphEdge.portalA, graphEdge.portalB)) {
                    return true;
                }
            }
            return false;
        };

        for (std::size_t vertexIndex = 0u; vertexIndex < cell.verticesXZ.size(); ++vertexIndex) {
            const glm::vec2& vertex = cell.verticesXZ[vertexIndex];
            const glm::vec2& previous = cell.verticesXZ[
                (vertexIndex + cell.verticesXZ.size() - 1u) % cell.verticesXZ.size()
            ];
            const glm::vec2& next = cell.verticesXZ[(vertexIndex + 1u) % cell.verticesXZ.size()];
            if (!edgeCoveredFromVertex(vertex, previous) || !edgeCoveredFromVertex(vertex, next)) {
                boundaryPoints.insert(quantizeLayerPoint(vertex, cell.elevationY));
            }
        }
    }

    for (std::size_t cellIndex = 0u; cellIndex < runtime.bakedCells.size(); ++cellIndex) {
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        std::vector<std::uint8_t>& flags = runtime.bakedCellBoundaryVertices[cellIndex];
        for (std::size_t vertexIndex = 0u; vertexIndex < cell.verticesXZ.size(); ++vertexIndex) {
            flags[vertexIndex] = boundaryPoints.contains(
                quantizeLayerPoint(cell.verticesXZ[vertexIndex], cell.elevationY)
            ) ? 1u : 0u;
        }
    }
}

int nextPolygonId(const NavMeshAsset& asset) {
    int nextId = 1;
    for (const NavPolygon& polygon : asset.polygons) {
        nextId = std::max(nextId, polygon.id + 1);
    }
    return nextId;
}

int nextLinkId(const NavMeshAsset& asset) {
    int nextId = 1;
    for (const NavLink& link : asset.links) {
        nextId = std::max(nextId, link.id + 1);
    }
    return nextId;
}

void setRuntimeStatus(NavigationRuntime& runtime, std::string message, bool isError) {
    runtime.statusMessage = std::move(message);
    runtime.statusIsError = isError;
}

std::optional<std::size_t> findPolygonIndexById(const NavigationRuntime& runtime, int polygonId) {
    const auto it = runtime.polygonIndexById.find(polygonId);
    if (it == runtime.polygonIndexById.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::size_t> findPolygonIndexById(const NavMeshAsset& asset, int polygonId) {
    for (std::size_t index = 0; index < asset.polygons.size(); ++index) {
        if (asset.polygons[index].id == polygonId) {
            return index;
        }
    }
    return std::nullopt;
}

glm::vec3 cellCenter3(const NavigationRuntime& runtime, std::size_t cellIndex) {
    const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
    const glm::vec2 center = cellIndex < runtime.bakedCellCenters.size()
        ? runtime.bakedCellCenters[cellIndex]
        : polygonCentroidXZ(cell.verticesXZ);
    return glm::vec3(center.x, cell.elevationY, center.y);
}

void updateSourceOverride(
    std::vector<NavSourceTagOverride>& overrides,
    const NavSourceComponent& source,
    NavSourceTag appliedTag
) {
    const auto it = std::find_if(overrides.begin(), overrides.end(), [&source](const NavSourceTagOverride& overrideRecord) {
        return overrideRecord.stableId == source.stableId;
    });
    if (appliedTag == source.defaultTag) {
        if (it != overrides.end()) {
            overrides.erase(it);
        }
        return;
    }

    if (it != overrides.end()) {
        it->tag = appliedTag;
        return;
    }
    overrides.push_back(NavSourceTagOverride{source.stableId, appliedTag});
}

bool shortestYawStep(float currentYaw, float desiredYaw, float maxStep, float& outYaw) {
    float delta = std::fmod(desiredYaw - currentYaw, 360.0f);
    if (delta > 180.0f) {
        delta -= 360.0f;
    } else if (delta < -180.0f) {
        delta += 360.0f;
    }
    if (std::abs(delta) <= maxStep) {
        outYaw = desiredYaw;
        return true;
    }
    outYaw = currentYaw + std::copysign(maxStep, delta);
    return false;
}

int findClipContaining(const render::GltfModelData& model, std::string_view token) {
    const std::string loweredToken = lowercaseCopy(std::string(token));
    for (std::size_t clipIndex = 0; clipIndex < model.animations.size(); ++clipIndex) {
        std::string lowered = lowercaseCopy(model.animations[clipIndex].name);
        if (lowered.find(loweredToken) != std::string::npos) {
            return static_cast<int>(clipIndex);
        }
    }
    return -1;
}

std::string resolveNavAssetPath(const std::string& assetPath) {
    if (assetPath.empty()) {
        return {};
    }
    const std::filesystem::path path(assetPath);
    if (path.is_absolute()) {
        return path.string();
    }

    char* basePath = SDL_GetBasePath();
    std::string resolved = basePath ? (std::filesystem::path(basePath) / path).string() : path.string();
    if (basePath != nullptr) {
        SDL_free(basePath);
    }
    return resolved;
}

}  // namespace

const char* navSourceTagName(NavSourceTag tag) {
    switch (tag) {
        case NavSourceTag::Walkable:
            return "Walkable";
        case NavSourceTag::Blocking:
            return "Blocking";
        case NavSourceTag::Ignored:
        default:
            return "Ignored";
    }
}

bool tryParseNavSourceTag(const std::string& token, NavSourceTag& outTag) {
    const std::string lowered = lowercaseCopy(token);
    if (lowered == "walkable") {
        outTag = NavSourceTag::Walkable;
        return true;
    }
    if (lowered == "blocking") {
        outTag = NavSourceTag::Blocking;
        return true;
    }
    if (lowered == "ignored") {
        outTag = NavSourceTag::Ignored;
        return true;
    }
    return false;
}

std::string serializeNavMeshAsset(const NavMeshAsset& asset) {
    std::ostringstream out;
    // max_digits10 guarantees an exact float round-trip. Four decimal places
    // were enough for display, but could move nearly-collinear navmesh edges
    // far enough to create sliver portals after a save/reload cycle.
    out << std::fixed << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "version " << asset.version << "\n";
    out << "minimum_runtime_cell_area "
        << std::max(asset.minimumRuntimeCellArea, 0.0f) << "\n";
    for (const NavSourceTagOverride& overrideRecord : asset.sourceTagOverrides) {
        out << "source_tag_override " << std::quoted(overrideRecord.stableId) << " " << navSourceTagName(overrideRecord.tag) << "\n";
    }
    for (const NavPolygon& polygon : asset.polygons) {
        out << "polygon " << polygon.id << " " << polygon.elevationY;
        for (const glm::vec2& vertex : polygon.verticesXZ) {
            out << " " << vertex.x << " " << vertex.y;
        }
        out << "\n";
    }
    for (const NavLink& link : asset.links) {
        out << "link " << link.id << " " << link.fromPolygonId << " " << link.toPolygonId
            << " "
            << link.fromPoint.x << " " << link.fromPoint.y << " " << link.fromPoint.z
            << " " << link.toPoint.x << " " << link.toPoint.y << " " << link.toPoint.z
            << " " << (link.bidirectional ? 1 : 0) << "\n";
    }
    return out.str();
}

bool parseNavMeshAsset(const std::string& text, NavMeshAsset& outAsset, std::string* error) {
    std::istringstream input(text);
    std::string line{};
    NavMeshAsset asset{};
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream lineStream(line);
        std::string keyword{};
        lineStream >> keyword;
        if (keyword == "version") {
            if (!(lineStream >> asset.version)) {
                if (error) {
                    *error = "Invalid version at line " + std::to_string(lineNumber);
                }
                return false;
            }
        } else if (keyword == "minimum_runtime_cell_area") {
            if (!(lineStream >> asset.minimumRuntimeCellArea) ||
                !std::isfinite(asset.minimumRuntimeCellArea) ||
                asset.minimumRuntimeCellArea < 0.0f) {
                if (error) {
                    *error = "Invalid minimum_runtime_cell_area at line " + std::to_string(lineNumber);
                }
                return false;
            }
        } else if (keyword == "source_tag_override") {
            std::string stableId{};
            std::string tagToken{};
            if (!(lineStream >> std::quoted(stableId) >> tagToken)) {
                if (error) {
                    *error = "Invalid source_tag_override at line " + std::to_string(lineNumber);
                }
                return false;
            }
            NavSourceTag tag = NavSourceTag::Ignored;
            if (!tryParseNavSourceTag(tagToken, tag)) {
                if (error) {
                    *error = "Unknown nav tag at line " + std::to_string(lineNumber);
                }
                return false;
            }
            asset.sourceTagOverrides.push_back(NavSourceTagOverride{stableId, tag});
        } else if (keyword == "polygon") {
            NavPolygon polygon{};
            if (!(lineStream >> polygon.id >> polygon.elevationY)) {
                if (error) {
                    *error = "Invalid polygon header at line " + std::to_string(lineNumber);
                }
                return false;
            }
            glm::vec2 vertex(0.0f);
            while (lineStream >> vertex.x >> vertex.y) {
                polygon.verticesXZ.push_back(vertex);
            }
            if (!polygonValid(polygon)) {
                if (error) {
                    *error = "Polygon must have at least three vertices at line " + std::to_string(lineNumber);
                }
                return false;
            }
            asset.polygons.push_back(std::move(polygon));
        } else if (keyword == "link") {
            NavLink link{};
            int bidirectional = 0;
            if (!(lineStream >> link.id >> link.fromPolygonId >> link.toPolygonId
                    >> link.fromPoint.x >> link.fromPoint.y >> link.fromPoint.z
                    >> link.toPoint.x >> link.toPoint.y >> link.toPoint.z
                    >> bidirectional)) {
                if (error) {
                    *error = "Invalid link at line " + std::to_string(lineNumber);
                }
                return false;
            }
            link.bidirectional = bidirectional != 0;
            asset.links.push_back(link);
        } else {
            if (error) {
                *error = "Unknown keyword '" + keyword + "' at line " + std::to_string(lineNumber);
            }
            return false;
        }
    }

    if (asset.version != kNavAssetVersion) {
        if (error) {
            *error = "Unsupported navmesh version " + std::to_string(asset.version);
        }
        return false;
    }

    outAsset = std::move(asset);
    return true;
}

bool NavigationSystem::initializeScene(const SceneBlueprint& blueprint, World& world, NavigationRuntime& runtime) const {
    runtime.assetPath = resolveNavAssetPath(blueprint.navMeshAssetPath);
    runtime.editor = NavigationEditorState{};
    const bool loaded = loadAsset(runtime);
    const ParentPathData paths = buildStableIdPaths(world);

    const std::unordered_map<std::string, NavSourceTag> overrideById = [&runtime]() {
        std::unordered_map<std::string, NavSourceTag> overrides{};
        overrides.reserve(runtime.asset.sourceTagOverrides.size());
        for (const NavSourceTagOverride& overrideRecord : runtime.asset.sourceTagOverrides) {
            overrides[overrideRecord.stableId] = overrideRecord.tag;
        }
        return overrides;
    }();

    for (EntityId entity : world.renderables.entities()) {
        const RenderableComponent& renderable = world.renderables.get(entity);
        const NavSourceTag defaultTag = defaultTagForLayer(renderable.layer);
        const auto pathIt = paths.pathByEntity.find(entity);
        const std::string stableId = pathIt != paths.pathByEntity.end()
            ? pathIt->second
            : ("Renderable/" + std::to_string(entity.index));
        const auto overrideIt = overrideById.find(stableId);
        const NavSourceTag effectiveTag = overrideIt != overrideById.end() ? overrideIt->second : defaultTag;
        world.navSources.emplace(entity, NavSourceComponent{stableId, defaultTag, effectiveTag});
    }

    for (EntityId entity : world.animatedModels.entities()) {
        const NameComponent* name = world.names.tryGet(entity);
        if (name == nullptr || name->value != "Character") {
            continue;
        }
        if (!world.navAgents.contains(entity)) {
            world.navAgents.emplace(entity, NavAgentComponent{});
        }
        const AnimatedModelComponent& animated = world.animatedModels.get(entity);
        if (animated.model) {
            const int idleClip = render::findDefaultAnimationClipIndex(*animated.model);
            int walkClip = findClipContaining(*animated.model, "walk");
            if (walkClip < 0) {
                walkClip = findClipContaining(*animated.model, "run");
            }
            world.locomotion.emplace(entity, LocomotionComponent{idleClip, walkClip});
        }
    }

    const bool rebuilt = rebuildRuntime(runtime);
    return loaded && rebuilt;
}

bool NavigationSystem::loadAsset(NavigationRuntime& runtime) const {
    invalidatePendingPathRequests();
    runtime.solveSnapshot.reset();
    runtime.asset = NavMeshAsset{};
    if (runtime.assetPath.empty()) {
        setRuntimeStatus(runtime, "No navmesh asset configured.", true);
        return false;
    }

    std::ifstream input(runtime.assetPath, std::ios::binary);
    if (!input.is_open()) {
        runtime.asset.version = kNavAssetVersion;
        setRuntimeStatus(runtime, "Navmesh asset missing; using an empty in-memory asset.", false);
        return true;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string error{};
    if (!parseNavMeshAsset(buffer.str(), runtime.asset, &error)) {
        runtime.asset = NavMeshAsset{};
        setRuntimeStatus(runtime, error, true);
        return false;
    }
    setRuntimeStatus(runtime, "Loaded navmesh asset.", false);
    return true;
}

bool NavigationSystem::reloadAsset(NavigationRuntime& runtime) const {
    if (!loadAsset(runtime)) {
        return false;
    }
    return rebuildRuntime(runtime);
}

bool NavigationSystem::saveAsset(const NavigationRuntime& runtime, std::string* error) const {
    if (runtime.assetPath.empty()) {
        if (error) {
            *error = "No navmesh asset path configured.";
        }
        return false;
    }

    std::error_code createError{};
    const std::filesystem::path path(runtime.assetPath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), createError);
        if (createError) {
            if (error) {
                *error = createError.message();
            }
            return false;
        }
    }

    std::ofstream output(runtime.assetPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        if (error) {
            *error = "Failed to open navmesh asset for writing.";
        }
        return false;
    }
    output << serializeNavMeshAsset(runtime.asset);
    return output.good();
}

bool NavigationSystem::rebuildRuntime(NavigationRuntime& runtime, std::string* error) const {
    return rebuildRuntimeInternal(runtime, error, false);
}

bool NavigationSystem::rebuildRuntimeInternal(
    NavigationRuntime& runtime,
    std::string* error,
    bool sourcePolygonsAreDisjoint
) const {
    invalidatePendingPathRequests();
    runtime.solveSnapshot.reset();
    runtime.polyanyaMesh.reset();
    runtime.exactPathfindingWarning.clear();
    runtime.bakedCellsHaveInteriorOverlap = false;
    ++runtime.solveRevision;
    runtime.polygonIndexById.clear();
    runtime.polygonCenters.clear();
    runtime.bakedCells.clear();
    runtime.bakedCellCenters.clear();
    runtime.bakedCellMinXZ.clear();
    runtime.bakedCellMaxXZ.clear();
    runtime.bakedCellBoundaryVertices.clear();
    runtime.polygonToCellIndices.clear();
    runtime.cellToPolygonIndices.clear();
    runtime.graph.clear();
    runtime.filteredRuntimeCellCount = 0u;

    std::vector<BakeLayerData> bakeLayers{};
    runtime.polygonToCellIndices.resize(runtime.asset.polygons.size());
    for (std::size_t index = 0; index < runtime.asset.polygons.size(); ++index) {
        const NavPolygon& polygon = runtime.asset.polygons[index];
        if (!polygonValid(polygon)) {
            if (error) {
                *error = "Invalid polygon geometry for polygon id " + std::to_string(polygon.id);
            }
            return false;
        }
        runtime.polygonIndexById[polygon.id] = index;
        runtime.polygonCenters.push_back(polygonCentroidXZ(polygon.verticesXZ));

        std::vector<glm::vec2> normalized = normalizePolygonVertices(polygon.verticesXZ);
        if (!polygonIsSimple(normalized)) {
            if (error) {
                *error = "Self-intersecting polygon geometry for polygon id " + std::to_string(polygon.id);
            }
            return false;
        }
        if (!polygonHasArea(normalized)) {
            if (error) {
                *error = "Degenerate polygon geometry for polygon id " + std::to_string(polygon.id);
            }
            return false;
        }
        const std::vector<std::array<glm::vec2, 3u>> triangles = triangulateSimplePolygon(normalized);
        if (triangles.empty()) {
            if (error) {
                *error = "Failed to triangulate polygon id " + std::to_string(polygon.id);
            }
            return false;
        }

        auto layerIt = std::find_if(bakeLayers.begin(), bakeLayers.end(), [&](const BakeLayerData& layer) {
            return std::abs(layer.elevationY - polygon.elevationY) <= kLayerGroupingEpsilon;
        });
        if (layerIt == bakeLayers.end()) {
            bakeLayers.push_back(BakeLayerData{polygon.elevationY});
            layerIt = bakeLayers.end() - 1;
        }

        layerIt->polygons.push_back(AuthoredBakePolygon{index, polygon.elevationY, normalized});
        for (const auto& triangle : triangles) {
            layerIt->triangles.push_back(BakedTriangle{
                index,
                polygon.elevationY,
                triangle
            });
        }
    }

    for (const BakeLayerData& layer : bakeLayers) {
        std::vector<std::vector<std::size_t>> layerCellToPolygonIndices{};
        const bool mayUseDisjointFastPath = sourcePolygonsAreDisjoint ||
            !bakeLayerHasInteriorPolygonOverlap(layer);
        std::vector<NavRuntimeCell> layerCells{};
        if (mayUseDisjointFastPath) {
            layerCells = bakeDisjointLayerRuntimeCells(
                layer,
                layerCellToPolygonIndices
            );
            std::vector<glm::vec2> layerMinXZ{};
            std::vector<glm::vec2> layerMaxXZ{};
            layerMinXZ.reserve(layerCells.size());
            layerMaxXZ.reserve(layerCells.size());
            for (const NavRuntimeCell& cell : layerCells) {
                const auto [minXZ, maxXZ] =
                    polygonBoundsXZ(cell.verticesXZ);
                layerMinXZ.push_back(minXZ);
                layerMaxXZ.push_back(maxXZ);
            }
            // The source hint accelerates the common generated-mesh case, but
            // never bypasses verification. If clipping/quantization produced
            // a real overlap, rebuild this layer as an explicit union.
            if (runtimeCellsHaveInteriorOverlap(
                    layerCells,
                    layerMinXZ,
                    layerMaxXZ)) {
                layerCells = bakeLayerRuntimeCells(
                    layer,
                    layerCellToPolygonIndices
                );
            }
        } else {
            layerCells = bakeLayerRuntimeCells(
                layer,
                layerCellToPolygonIndices
            );
        }
        for (std::size_t localCellIndex = 0; localCellIndex < layerCells.size(); ++localCellIndex) {
            const std::vector<glm::vec2>& cellVertices =
                layerCells[localCellIndex].verticesXZ;
            const float cellArea = std::abs(polygonSignedArea(cellVertices));
            float longestEdge = 0.0f;
            for (std::size_t vertexIndex = 0u;
                 vertexIndex < cellVertices.size();
                 ++vertexIndex) {
                longestEdge = std::max(
                    longestEdge,
                    glm::distance(
                        cellVertices[vertexIndex],
                        cellVertices[
                            (vertexIndex + 1u) % cellVertices.size()]
                    )
                );
            }
            const float minimumAltitude = longestEdge > kPlaneEpsilon
                ? (2.0f * cellArea) / longestEdge
                : 0.0f;
            if (cellArea <
                    std::max(runtime.asset.minimumRuntimeCellArea, 0.0f) ||
                minimumAltitude <= kPortalBroadPhaseEpsilon) {
                ++runtime.filteredRuntimeCellCount;
                continue;
            }
            const std::size_t globalCellIndex = runtime.bakedCells.size();
            const auto [cellMinXZ, cellMaxXZ] = polygonBoundsXZ(layerCells[localCellIndex].verticesXZ);
            runtime.bakedCellCenters.push_back(polygonCentroidXZ(layerCells[localCellIndex].verticesXZ));
            runtime.bakedCellMinXZ.push_back(cellMinXZ);
            runtime.bakedCellMaxXZ.push_back(cellMaxXZ);
            runtime.bakedCells.push_back(std::move(layerCells[localCellIndex]));
            runtime.cellToPolygonIndices.push_back(layerCellToPolygonIndices[localCellIndex]);
            for (std::size_t polygonIndex : runtime.cellToPolygonIndices.back()) {
                if (polygonIndex < runtime.polygonToCellIndices.size()) {
                    runtime.polygonToCellIndices[polygonIndex].push_back(globalCellIndex);
                }
            }
        }
    }

    runtime.bakedCellsHaveInteriorOverlap = runtimeCellsHaveInteriorOverlap(
        runtime.bakedCells,
        runtime.bakedCellMinXZ,
        runtime.bakedCellMaxXZ
    );
    runtime.graph.resize(runtime.bakedCells.size());
    std::vector<std::size_t> graphSweepOrder(runtime.bakedCells.size());
    for (std::size_t cellIndex = 0u;
         cellIndex < graphSweepOrder.size();
         ++cellIndex) {
        graphSweepOrder[cellIndex] = cellIndex;
    }
    std::sort(
        graphSweepOrder.begin(),
        graphSweepOrder.end(),
        [&](std::size_t lhs, std::size_t rhs) {
            if (runtime.bakedCellMinXZ[lhs].x !=
                runtime.bakedCellMinXZ[rhs].x) {
                return runtime.bakedCellMinXZ[lhs].x <
                    runtime.bakedCellMinXZ[rhs].x;
            }
            return lhs < rhs;
        }
    );
    for (std::size_t lhsOrder = 0u;
         lhsOrder < graphSweepOrder.size();
         ++lhsOrder) {
        const std::size_t lhsIndex = graphSweepOrder[lhsOrder];
        const NavRuntimeCell& lhs = runtime.bakedCells[lhsIndex];
        for (std::size_t rhsOrder = lhsOrder + 1u;
             rhsOrder < graphSweepOrder.size();
             ++rhsOrder) {
            const std::size_t rhsIndex = graphSweepOrder[rhsOrder];
            if (runtime.bakedCellMinXZ[rhsIndex].x >
                runtime.bakedCellMaxXZ[lhsIndex].x +
                    kPortalBroadPhaseEpsilon) {
                break;
            }
            const NavRuntimeCell& rhs = runtime.bakedCells[rhsIndex];
            if (std::abs(lhs.elevationY - rhs.elevationY) >
                    kLayerGroupingEpsilon ||
                runtime.bakedCellMinXZ[rhsIndex].y >
                    runtime.bakedCellMaxXZ[lhsIndex].y +
                        kPortalBroadPhaseEpsilon ||
                runtime.bakedCellMaxXZ[rhsIndex].y <
                    runtime.bakedCellMinXZ[lhsIndex].y -
                        kPortalBroadPhaseEpsilon) {
                continue;
            }
            const std::vector<SharedPortalResult> portals =
                sharedBoundaryPortals(lhs, rhs);
            if (portals.empty()) {
                continue;
            }
            for (const SharedPortalResult& portal : portals) {
                runtime.graph[lhsIndex].push_back(NavGraphEdge{
                    rhsIndex,
                    false,
                    -1,
                    portal.b,
                    portal.a,
                    glm::vec3(0.0f),
                    glm::vec3(0.0f)
                });
                runtime.graph[rhsIndex].push_back(NavGraphEdge{
                    lhsIndex,
                    false,
                    -1,
                    portal.a,
                    portal.b,
                    glm::vec3(0.0f),
                    glm::vec3(0.0f)
                });
            }
        }
    }

    rebuildCellBoundaryVertexCache(runtime);

    const NavigationSolveView solveView = makeSolveView(runtime);
    for (const NavLink& link : runtime.asset.links) {
        const auto fromIndex = findPolygonIndexById(solveView, link.fromPolygonId);
        const auto toIndex = findPolygonIndexById(solveView, link.toPolygonId);
        if (!fromIndex.has_value() || !toIndex.has_value()) {
            continue;
        }
        std::vector<std::size_t> fromCells = findLinkEndpointCells(solveView, *fromIndex, link.fromPoint);
        std::vector<std::size_t> toCells = findLinkEndpointCells(solveView, *toIndex, link.toPoint);
        if (fromCells.empty()) {
            if (const auto fallback = findNearestCandidateCell(solveView, runtime.polygonToCellIndices[*fromIndex], link.fromPoint);
                fallback.has_value()) {
                fromCells.push_back(*fallback);
            }
        }
        if (toCells.empty()) {
            if (const auto fallback = findNearestCandidateCell(solveView, runtime.polygonToCellIndices[*toIndex], link.toPoint);
                fallback.has_value()) {
                toCells.push_back(*fallback);
            }
        }
        for (std::size_t fromCellIndex : fromCells) {
            for (std::size_t toCellIndex : toCells) {
                appendGraphEdgeIfMissing(runtime.graph[fromCellIndex], NavGraphEdge{
                    toCellIndex,
                    true,
                    link.id,
                    glm::vec2(0.0f),
                    glm::vec2(0.0f),
                    link.fromPoint,
                    link.toPoint
                });
                if (link.bidirectional) {
                    appendGraphEdgeIfMissing(runtime.graph[toCellIndex], NavGraphEdge{
                        fromCellIndex,
                        true,
                        link.id,
                        glm::vec2(0.0f),
                        glm::vec2(0.0f),
                        link.toPoint,
                        link.fromPoint
                    });
                }
            }
        }
    }
    std::string polyanyaError{};
    if (runtime.bakedCellsHaveInteriorOverlap) {
        polyanyaError =
            "runtime cells still have positive-area interior overlap";
    } else {
        runtime.polyanyaMesh = navigation_detail::buildPolyanyaMesh(
            runtime.bakedCells,
            runtime.graph,
            &polyanyaError
        );
    }
    if (runtime.polyanyaMesh == nullptr) {
        runtime.exactPathfindingWarning =
            "Exact Polyanya pathfinding unavailable: " + polyanyaError +
            ". Using the validated A* + funnel fallback.";
    }
    runtime.solveSnapshot = buildSolveSnapshot(runtime);
    return true;
}

bool NavigationSystem::generateFromTags(const World& world, NavigationRuntime& runtime, std::string* error) const {
    std::vector<LayerBuildData> layers{};

    for (EntityId entity : world.navSources.entities()) {
        const NavSourceComponent& source = world.navSources.get(entity);
        const NavSourceGeometryComponent* geometry = world.navSourceGeometry.tryGet(entity);
        if (geometry == nullptr || !geometry->mesh) {
            continue;
        }
        if (entity.index >= world.transformCache_.size()) {
            continue;
        }
        const glm::mat4 worldMatrix = world.transformCache_[entity.index].worldMatrix;
        const render::Mesh& mesh = *geometry->mesh;
        if (mesh.indices.size() % 3u != 0u) {
            continue;
        }

        if (source.effectiveTag == NavSourceTag::Walkable) {
            for (std::size_t index = 0; index + 2u < mesh.indices.size(); index += 3u) {
                const unsigned int ia = mesh.indices[index];
                const unsigned int ib = mesh.indices[index + 1u];
                const unsigned int ic = mesh.indices[index + 2u];
                if (ia >= mesh.positions.size() || ib >= mesh.positions.size() || ic >= mesh.positions.size()) {
                    continue;
                }

                const glm::vec3 a = transformPoint3(worldMatrix, mesh.positions[ia]);
                const glm::vec3 b = transformPoint3(worldMatrix, mesh.positions[ib]);
                const glm::vec3 c = transformPoint3(worldMatrix, mesh.positions[ic]);
                const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
                if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z)) {
                    continue;
                }
                if (normal.y < kHorizontalNormalMinDot) {
                    continue;
                }
                const float elevation = (a.y + b.y + c.y) / 3.0f;
                addTriangleToLayer(
                    layers,
                    elevation,
                    WalkableTriangle{
                        elevation,
                        glm::vec2(a.x, a.z),
                        glm::vec2(b.x, b.z),
                        glm::vec2(c.x, c.z)
                    }
                );
            }
        }
    }

    std::vector<BlockingFootprint> blockers = buildBlockingFootprints(world);
    for (LayerBuildData& layer : layers) {
        layer.blockers = blockers;
    }

    runtime.asset.polygons.clear();
    runtime.asset.links.clear();
    int polygonId = 1;
    std::sort(layers.begin(), layers.end(), [](const LayerBuildData& lhs, const LayerBuildData& rhs) {
        return lhs.elevationY < rhs.elevationY;
    });
    for (const LayerBuildData& layer : layers) {
        std::vector<NavPolygon> polygons = buildPolygonsForLayer(layer, polygonId);
        runtime.asset.polygons.insert(
            runtime.asset.polygons.end(),
            std::make_move_iterator(polygons.begin()),
            std::make_move_iterator(polygons.end())
        );
    }

    if (!rebuildRuntimeInternal(runtime, error, true)) {
        return false;
    }
    setRuntimeStatus(
        runtime,
        "Generated " + std::to_string(runtime.asset.polygons.size()) + " navmesh polygons from hitbox unions.",
        false
    );
    return true;
}

std::optional<NavHitResult> NavigationSystem::hitTest(
    const NavigationRuntime& runtime,
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight,
    int mouseX,
    int mouseY
) const {
    const std::optional<ViewportRay> ray = makeViewportRay(camera, viewportWidth, viewportHeight, mouseX, mouseY);
    if (!ray.has_value()) {
        return std::nullopt;
    }

    std::optional<NavHitResult> bestHit{};
    for (const NavPolygon& polygon : runtime.asset.polygons) {
        if (std::abs(ray->direction.y) <= kPlaneEpsilon) {
            continue;
        }
        const float t = (polygon.elevationY - ray->origin.y) / ray->direction.y;
        if (t < 0.0f) {
            continue;
        }
        const glm::vec3 point = ray->origin + ray->direction * t;
        if (!pointInPolygonXZ(glm::vec2(point.x, point.z), polygon.verticesXZ)) {
            continue;
        }
        if (!bestHit.has_value() || t < bestHit->distance) {
            bestHit = NavHitResult{polygon.id, point, t};
        }
    }
    return bestHit;
}

bool NavigationSystem::setAgentDestination(
    World& world,
    const NavigationRuntime& runtime,
    EntityId agentEntity,
    const glm::vec3& destination
) const {
    ProfilerService::CpuScopeHandle profileScope{};
    if (profiler_ != nullptr) {
        profileScope = profiler_->scopedCpu("Navigation Pathfind");
    }

    NavAgentComponent* agent = world.navAgents.tryGet(agentEntity);
    TransformComponent* transform = world.transforms.tryGet(agentEntity);
    const std::shared_ptr<const NavigationSolveSnapshot> snapshot = runtime.solveSnapshot;
    if (agent == nullptr || transform == nullptr || snapshot == nullptr || snapshot->bakedCells.empty()) {
        return false;
    }

    const AgentClearanceProfile clearanceProfile = resolveAgentClearanceProfile(world, agentEntity, *agent);
    const std::optional<SolvedPath> path =
        solvePathCorners(makeSolveView(*snapshot), transform->position, destination, agent->arrivalRadius, clearanceProfile);
    if (!path.has_value()) {
        return false;
    }

    discardPendingPathRequest(agentEntity);
    snapAgentToResolvedStart(
        world,
        agentEntity,
        *transform,
        path->resolvedStart
    );
    applyPathResult(*agent, path->destination, std::move(path->corners));
    return true;
}

bool NavigationSystem::requestAgentDestination(
    World& world,
    const NavigationRuntime& runtime,
    TaskScheduler& scheduler,
    EntityId agentEntity,
    const glm::vec3& destination
) const {
    NavAgentComponent* agent = world.navAgents.tryGet(agentEntity);
    TransformComponent* transform = world.transforms.tryGet(agentEntity);
    const std::shared_ptr<const NavigationSolveSnapshot> snapshot = runtime.solveSnapshot;
    if (agent == nullptr || transform == nullptr || snapshot == nullptr || snapshot->bakedCells.empty()) {
        return false;
    }

    const glm::vec3 startPosition = transform->position;
    const float arrivalRadius = agent->arrivalRadius;
    const AgentClearanceProfile clearanceProfile = resolveAgentClearanceProfile(world, agentEntity, *agent);
    const std::uint64_t requestId = nextPathRequestId_++;
    const std::uint64_t solveRevision = runtime.solveRevision;
    std::shared_ptr<PendingPathProgress> progress = std::make_shared<PendingPathProgress>();
    auto cancelled = std::make_shared<std::atomic<bool>>(false);

    discardPendingPathRequest(agentEntity);
    pendingPathRequests_[agentEntity] = PendingPathRequest{
        requestId,
        solveRevision,
        startPosition,
        destination,
        progress,
        cancelled,
        false,
        scheduler.submitAsync("Navigation Pathfind", [
            snapshot,
            agentEntity,
            requestId,
            solveRevision,
            startPosition,
            destination,
            arrivalRadius,
            clearanceProfile,
            progress,
            cancelled
        ]() {
            PathSolveResult result{};
            result.agentEntity = agentEntity;
            result.requestId = requestId;
            result.solveRevision = solveRevision;
            result.startPosition = startPosition;
            result.destination = destination;
            if (cancelled->load(std::memory_order_acquire)) {
                return result;
            }
            const auto startedAt = std::chrono::steady_clock::now();
            const NavigationSolveView solveView = makeSolveView(*snapshot);
            if (cancelled->load(std::memory_order_acquire)) {
                return result;
            }
            std::optional<SolvedPath> path =
                solvePathCorners(
                    solveView,
                    startPosition,
                    destination,
                    arrivalRadius,
                    clearanceProfile,
                    cancelled.get()
                );
            if (cancelled->load(std::memory_order_acquire)) {
                return result;
            }
            if (path.has_value()) {
                result.resolvedStart = path->resolvedStart;
                result.destination = path->destination;
                result.pathCorners = path->corners;
                // Publish partial path so the agent can start moving toward the
                // destination before the final result is applied.
                {
                    std::lock_guard<std::mutex> lock(progress->mutex);
                    progress->partialPath = PartialPathResult{
                        path->resolvedStart,
                        path->destination,
                        path->corners
                    };
                }
            } else {
                result.pathCorners.reset();
            }
            result.durationUs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startedAt).count()
            );
            return result;
        })
    };
    return true;
}

void NavigationSystem::applyCompletedPathRequests(World& world, const NavigationRuntime& runtime) const {
    for (auto it = pendingPathRequests_.begin(); it != pendingPathRequests_.end();) {
        PendingPathRequest& pending = it->second;
        if (!pending.handle.valid()) {
            it = pendingPathRequests_.erase(it);
            continue;
        }
        // Apply partial path before the final result so the agent has a destination
        // while the async solve is in progress (or just completed).
        if (!pending.partialPathApplied) {
            auto partial = consumePartialPathResult(pending.progress);
            if (partial.has_value()) {
                NavAgentComponent* agent = world.navAgents.tryGet(it->first);
                TransformComponent* transform =
                    world.transforms.tryGet(it->first);
                if (agent != nullptr && transform != nullptr) {
                    const AgentClearanceProfile clearanceProfile =
                        resolveAgentClearanceProfile(
                            world,
                            it->first,
                            *agent
                        );
                    const bool agentHasNotMoved = nearlyEqualVec3(
                        transform->position,
                        pending.startPosition,
                        kPolygonEpsilon * 8.0f
                    );
                    if (agentHasNotMoved) {
                        snapAgentToResolvedStart(
                            world,
                            it->first,
                            *transform,
                            partial->resolvedStart
                        );
                    } else if (runtime.solveSnapshot != nullptr) {
                        partial->pathCorners =
                            trimPathCornersFromCurrentPosition(
                                makeSolveView(*runtime.solveSnapshot),
                                transform->position,
                                std::move(partial->pathCorners),
                                agent->arrivalRadius,
                                clearanceProfile
                            );
                    }
                    const bool pathStillValid =
                        runtime.solveSnapshot != nullptr &&
                        pathSegmentsAreValid(
                            makeSolveView(*runtime.solveSnapshot),
                            transform->position,
                            partial->pathCorners,
                            clearanceProfile
                        );
                    if (pathStillValid) {
                        applyPathResult(
                            *agent,
                            partial->destination,
                            std::move(partial->pathCorners)
                        );
                        pending.partialPathApplied = true;
                    }
                }
                if (pending.partialPathApplied) {
                    ++it;
                    continue;
                }
            }
        }
        if (!pending.handle.ready()) {
            ++it;
            continue;
        }

        std::optional<PathSolveResult> result{};
        try {
            result = pending.handle.take();
        } catch (...) {
            ++failedPathRequests_;
            it = pendingPathRequests_.erase(it);
            continue;
        }
        if (!result.has_value()) {
            it = pendingPathRequests_.erase(it);
            continue;
        }

        lastAsyncPathfindUs_ = result->durationUs;

        const bool stale =
            pending.requestId != result->requestId ||
            pending.solveRevision != result->solveRevision ||
            runtime.solveSnapshot == nullptr ||
            result->solveRevision != runtime.solveRevision ||
            !world.isAlive(result->agentEntity);
        if (stale) {
            ++stalePathResults_;
            it = pendingPathRequests_.erase(it);
            continue;
        }

        NavAgentComponent* agent = world.navAgents.tryGet(result->agentEntity);
        TransformComponent* transform = world.transforms.tryGet(result->agentEntity);
        if (agent == nullptr || transform == nullptr) {
            ++stalePathResults_;
            it = pendingPathRequests_.erase(it);
            continue;
        }

        if (!result->pathCorners.has_value()) {
            ++failedPathRequests_;
            it = pendingPathRequests_.erase(it);
            continue;
        }

        const AgentClearanceProfile clearanceProfile =
            resolveAgentClearanceProfile(world, result->agentEntity, *agent);
        const NavigationSolveView solveView =
            makeSolveView(*runtime.solveSnapshot);
        const bool agentHasNotMoved = nearlyEqualVec3(
            transform->position,
            result->startPosition,
            kPolygonEpsilon * 8.0f
        );
        if (!pending.partialPathApplied && agentHasNotMoved) {
            snapAgentToResolvedStart(
                world,
                result->agentEntity,
                *transform,
                result->resolvedStart
            );
        }
        std::vector<glm::vec3> trimmedCorners =
            trimPathCornersFromCurrentPosition(
                solveView,
                transform->position,
                *result->pathCorners,
                agent->arrivalRadius,
                clearanceProfile
            );
        if (!pathSegmentsAreValid(
                solveView,
                transform->position,
                trimmedCorners,
                clearanceProfile)) {
            // The agent moved far enough that the completed route can no
            // longer be joined safely. Never re-run the expensive solver on
            // the main thread while draining asynchronous results.
            ++stalePathResults_;
            it = pendingPathRequests_.erase(it);
            continue;
        }
        applyPathResult(*agent, result->destination, std::move(trimmedCorners));
        it = pendingPathRequests_.erase(it);
    }
}

std::vector<render::FrameCounterRecord> NavigationSystem::profilingCounters() const {
    return {
        makeNavigationCounter("Pending Path Requests", static_cast<std::int64_t>(pendingPathRequests_.size())),
        makeNavigationCounter("Last Async Pathfind Us", static_cast<std::int64_t>(lastAsyncPathfindUs_)),
        makeNavigationCounter("Failed Path Requests", static_cast<std::int64_t>(failedPathRequests_)),
        makeNavigationCounter("Stale Path Results", static_cast<std::int64_t>(stalePathResults_)),
    };
}

void NavigationSystem::discardPendingPathRequest(EntityId entity) const {
    const auto it = pendingPathRequests_.find(entity);
    if (it == pendingPathRequests_.end()) {
        return;
    }
    if (it->second.cancelled) {
        it->second.cancelled->store(true, std::memory_order_release);
    }
    ++stalePathResults_;
    pendingPathRequests_.erase(it);
}

void NavigationSystem::invalidatePendingPathRequests() const {
    for (auto& [entity, pending] : pendingPathRequests_) {
        if (pending.cancelled) {
            pending.cancelled->store(true, std::memory_order_release);
        }
    }
    stalePathResults_ += pendingPathRequests_.size();
    pendingPathRequests_.clear();
}

void NavigationSystem::updateAgents(World& world, const NavigationRuntime&, const TimeContext& time) const {
    for (EntityId entity : world.navAgents.entities()) {
        NavAgentComponent& agent = world.navAgents.get(entity);
        TransformComponent* transform = world.transforms.tryGet(entity);
        if (transform == nullptr) {
            continue;
        }

        AnimatedModelComponent* animation = world.animatedModels.tryGet(entity);
        LocomotionComponent* locomotion = world.locomotion.tryGet(entity);

        if (!agent.moving || agent.pathCorners.empty()) {
            agent.moving = false;
            if (animation != nullptr && locomotion != nullptr && locomotion->idleClip >= 0) {
                const int currentOrNext = animation->nextClip >= 0 ? animation->nextClip : animation->currentClip;
                if (currentOrNext != locomotion->idleClip) {
                    animation->requestedClip = locomotion->idleClip;
                }
            }
            continue;
        }

        const glm::vec3 target = agent.pathCorners.front();
        glm::vec3 toTarget = target - transform->position;
        const float distance = glm::length(toTarget);
        if (distance <= agent.arrivalRadius) {
            transform->position = target;
            agent.pathCorners.erase(agent.pathCorners.begin());
            world.markTransformsDirty(entity);
            if (agent.pathCorners.empty()) {
                agent.destination.reset();
                agent.moving = false;
            }
            continue;
        }

        const glm::vec3 direction = toTarget / std::max(distance, kPlaneEpsilon);
        const glm::vec2 planarDirection(direction.x, direction.z);
        if (glm::dot(planarDirection, planarDirection) > kPlaneEpsilon) {
            const float desiredYaw = glm::degrees(
                std::atan2(planarDirection.x, planarDirection.y)
            );
            const AgentClearanceProfile clearanceProfile =
                resolveAgentClearanceProfile(world, entity, agent);
            if (clearanceProfile.shape == AgentClearanceShape::Box) {
                // Box paths are validated with the longitudinal axis aligned
                // to each segment. Align before translating so movement and
                // collision clearance use the same orientation model.
                transform->rotationDeg.y = desiredYaw;
            } else {
                float updatedYaw = transform->rotationDeg.y;
                shortestYawStep(
                    transform->rotationDeg.y,
                    desiredYaw,
                    agent.turnSpeedDeg * time.deltaSeconds,
                    updatedYaw
                );
                transform->rotationDeg.y = updatedYaw;
            }
        }

        const float step = agent.moveSpeed * time.deltaSeconds;
        if (step >= distance) {
            transform->position = target;
            agent.pathCorners.erase(agent.pathCorners.begin());
            if (agent.pathCorners.empty()) {
                agent.destination.reset();
                agent.moving = false;
            }
        } else {
            transform->position += direction * step;
        }

        world.markTransformsDirty(entity);
        if (animation != nullptr && locomotion != nullptr && locomotion->walkClip >= 0) {
            const int currentOrNext = animation->nextClip >= 0 ? animation->nextClip : animation->currentClip;
            if (currentOrNext != locomotion->walkClip) {
                animation->requestedClip = locomotion->walkClip;
            }
            animation->speed = 1.0f;
        }
    }
}

void NavigationSystem::syncFrame(const World& world, const NavigationRuntime& runtime, FrameSceneData& frame) const {
    frame.navigation.clear();
    frame.navigation.polygons.reserve(runtime.asset.polygons.size());
    frame.navigation.links.reserve(runtime.asset.links.size());
    for (const NavPolygon& polygon : runtime.asset.polygons) {
        FrameNavDebugPolygon debugPolygon{};
        debugPolygon.id = polygon.id;
        debugPolygon.elevationY = polygon.elevationY;
        debugPolygon.color = kWalkableOverlayColor;
        debugPolygon.vertices.reserve(polygon.verticesXZ.size());
        for (const glm::vec2& vertex : polygon.verticesXZ) {
            debugPolygon.vertices.push_back(glm::vec3(vertex.x, polygon.elevationY, vertex.y));
        }
        frame.navigation.polygons.push_back(std::move(debugPolygon));
    }
    for (const NavLink& link : runtime.asset.links) {
        frame.navigation.links.push_back(FrameNavDebugLink{
            link.id,
            link.fromPoint,
            link.toPoint,
            link.bidirectional
        });
    }
    for (EntityId entity : world.navAgents.entities()) {
        const NavAgentComponent& agent = world.navAgents.get(entity);
        frame.navigation.path = agent.pathCorners;
        frame.navigation.destination = agent.destination;
        break;
    }
    if (runtime.editor.polygonCaptureActive) {
        frame.navigation.captureElevationY = runtime.editor.polygonCaptureElevation;
        for (const glm::vec2& vertex : runtime.editor.polygonCaptureVertices) {
            frame.navigation.captureVertices.push_back(glm::vec3(vertex.x, runtime.editor.polygonCaptureElevation, vertex.y));
        }
    }
}

std::vector<EntityId> NavigationSystem::collectRenderableSelectionTargets(const World& world, EntityId selected) const {
    std::vector<EntityId> targets{};
    if (!selected.valid() || !world.isAlive(selected)) {
        return targets;
    }

    if (world.navSources.contains(selected)) {
        targets.push_back(selected);
        return targets;
    }

    const auto visit = [&](const auto& self, EntityId entity) -> void {
        for (EntityId child : world.parents.entities()) {
            const ParentComponent* parent = world.parents.tryGet(child);
            if (parent == nullptr || parent->parent != entity) {
                continue;
            }
            if (world.navSources.contains(child)) {
                targets.push_back(child);
            }
            self(self, child);
        }
    };
    visit(visit, selected);
    return targets;
}

void NavigationSystem::applyTagOverride(World& world, NavigationRuntime& runtime, EntityId entity, NavSourceTag tag) const {
    NavSourceComponent* source = world.navSources.tryGet(entity);
    if (source == nullptr) {
        return;
    }
    source->effectiveTag = tag;
    updateSourceOverride(runtime.asset.sourceTagOverrides, *source, tag);
}

bool NavigationSystem::capturePolygonClick(
    NavigationRuntime& runtime,
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight,
    int mouseX,
    int mouseY
) const {
    const std::optional<ViewportRay> ray = makeViewportRay(camera, viewportWidth, viewportHeight, mouseX, mouseY);
    if (!ray.has_value() || std::abs(ray->direction.y) <= kPlaneEpsilon) {
        return false;
    }

    const float t = (runtime.editor.polygonCaptureElevation - ray->origin.y) / ray->direction.y;
    if (t < 0.0f) {
        return false;
    }
    const glm::vec3 worldPoint = ray->origin + ray->direction * t;
    const glm::vec2 pointXZ(worldPoint.x, worldPoint.z);
    if (!runtime.editor.polygonCaptureVertices.empty() &&
        nearlyEqualVec2(runtime.editor.polygonCaptureVertices.back(), pointXZ, 0.01f)) {
        return false;
    }
    runtime.editor.polygonCaptureVertices.push_back(pointXZ);
    return true;
}

bool NavigationSystem::commitCapturedPolygon(NavigationRuntime& runtime, std::string* error) const {
    if (runtime.editor.polygonCaptureVertices.size() < 3u) {
        if (error) {
            *error = "Need at least three captured vertices.";
        }
        return false;
    }

    runtime.asset.polygons.push_back(NavPolygon{
        nextPolygonId(runtime.asset),
        runtime.editor.polygonCaptureElevation,
        runtime.editor.polygonCaptureVertices
    });
    runtime.editor.polygonCaptureVertices.clear();
    runtime.editor.polygonCaptureActive = false;
    return rebuildRuntime(runtime, error);
}

void NavigationSystem::clearCapturedPolygon(NavigationRuntime& runtime) const {
    runtime.editor.polygonCaptureVertices.clear();
    runtime.editor.polygonCaptureActive = false;
}

bool NavigationSystem::seedPendingLink(NavigationRuntime& runtime, int fromPolygonId, int toPolygonId, std::string* error) const {
    const auto fromIndex = findPolygonIndexById(runtime, fromPolygonId);
    const auto toIndex = findPolygonIndexById(runtime, toPolygonId);
    if (!fromIndex.has_value() || !toIndex.has_value()) {
        if (error) {
            *error = "Select valid polygons before seeding a link.";
        }
        return false;
    }

    const NavPolygon& fromPolygon = runtime.asset.polygons[*fromIndex];
    const NavPolygon& toPolygon = runtime.asset.polygons[*toIndex];
    float bestDistance = std::numeric_limits<float>::max();
    glm::vec2 bestFrom = fromPolygon.verticesXZ.front();
    glm::vec2 bestTo = toPolygon.verticesXZ.front();
    for (const glm::vec2& fromVertex : fromPolygon.verticesXZ) {
        for (const glm::vec2& toVertex : toPolygon.verticesXZ) {
            const float distance = glm::distance(fromVertex, toVertex);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestFrom = fromVertex;
                bestTo = toVertex;
            }
        }
    }

    runtime.editor.pendingLinkFromPolygonId = fromPolygonId;
    runtime.editor.pendingLinkToPolygonId = toPolygonId;
    runtime.editor.pendingLinkFromPoint = glm::vec3(bestFrom.x, fromPolygon.elevationY, bestFrom.y);
    runtime.editor.pendingLinkToPoint = glm::vec3(bestTo.x, toPolygon.elevationY, bestTo.y);
    runtime.editor.pendingLinkBidirectional = true;
    return true;
}

bool NavigationSystem::commitPendingLink(NavigationRuntime& runtime, std::string* error) const {
    if (runtime.editor.pendingLinkFromPolygonId < 0 || runtime.editor.pendingLinkToPolygonId < 0) {
        if (error) {
            *error = "Select source and target polygons first.";
        }
        return false;
    }

    runtime.asset.links.push_back(NavLink{
        nextLinkId(runtime.asset),
        runtime.editor.pendingLinkFromPolygonId,
        runtime.editor.pendingLinkToPolygonId,
        runtime.editor.pendingLinkFromPoint,
        runtime.editor.pendingLinkToPoint,
        runtime.editor.pendingLinkBidirectional
    });
    runtime.editor.pendingLinkFromPolygonId = -1;
    runtime.editor.pendingLinkToPolygonId = -1;
    runtime.editor.pendingLinkFromPoint = glm::vec3(0.0f);
    runtime.editor.pendingLinkToPoint = glm::vec3(0.0f);
    runtime.editor.pendingLinkBidirectional = true;
    return rebuildRuntime(runtime, error);
}

}  // namespace core
