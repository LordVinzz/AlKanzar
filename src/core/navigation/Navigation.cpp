#include "Navigation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
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
constexpr float kPolygonEpsilon = 1.0e-4f;
constexpr float kPlaneEpsilon = 1.0e-5f;
constexpr float kTau = 6.283185307179586f;
constexpr int kClearanceSampleDirections = 24;
constexpr int kSegmentClearanceSampleDirections = 8;
constexpr int kClearanceBinarySearchSteps = 10;
constexpr int kClearanceProjectionIterations = 16;
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
};

struct SolvedPath {
    glm::vec3 destination{0.0f};
    std::vector<glm::vec3> corners{};
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
    bool keepLeft
);
std::string canonicalPolygonKey(const std::vector<glm::vec2>& vertices);
std::vector<glm::vec2> buildConvexHull(std::vector<glm::vec2> points);
void mergeAdjacentConvexCells(std::vector<NavRuntimeCell>& cells);

float cross2(const glm::vec2& lhs, const glm::vec2& rhs) {
    return lhs.x * rhs.y - lhs.y * rhs.x;
}

float triArea2(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    return cross2(b - a, c - a);
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
    const float clearanceDistance = supportDistance(profile, direction, travelDirection);
    if (length <= clearanceDistance * 2.0f + kPolygonEpsilon) {
        return std::nullopt;
    }
    return SharedPortalResult{
        a + direction * clearanceDistance,
        b - direction * clearanceDistance
    };
}

bool cellsShareAuthoredPolygon(
    const NavigationSolveView& runtime,
    std::size_t cellA,
    std::size_t cellB
) {
    if (cellA >= runtime.cellToPolygonIndices.size() || cellB >= runtime.cellToPolygonIndices.size()) {
        return false;
    }
    const std::vector<std::size_t>& polyA = runtime.cellToPolygonIndices[cellA];
    const std::vector<std::size_t>& polyB = runtime.cellToPolygonIndices[cellB];
    for (std::size_t indexA : polyA) {
        for (std::size_t indexB : polyB) {
            if (indexA == indexB) {
                return true;
            }
        }
    }
    return false;
}

// Forward declarations for functions defined later in the file
bool pointInsideAuthoredWalkableSurface(const NavigationSolveView& runtime, const glm::vec3& point);
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

    for (int sampleIndex = 0; sampleIndex < sampleDirections; ++sampleIndex) {
        const glm::vec2 direction = clearanceSampleDirection(sampleIndex, sampleDirections);
        const float clearanceDistance = supportDistance(profile, direction, travelDirection);
        const glm::vec3 samplePoint(
            point.x + direction.x * clearanceDistance,
            point.y,
            point.z + direction.y * clearanceDistance
        );
        if (!pointInsideAuthoredWalkableSurface(runtime, samplePoint)) {
            return false;
        }
    }
    return true;
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
    const glm::vec2& preferredTravelDirection = glm::vec2(0.0f, 1.0f)
) {
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
        resolved = cellCenter3(runtime, *nearest);
    }
    const glm::vec2 inwardDirection = travelDirectionForSegment(point, resolved, preferredTravelDirection);
    if (pointInsideAuthoredWalkableSurfaceWithClearance(runtime, resolved, profile, inwardDirection)) {
        return resolved;
    }

    for (int iteration = 0; iteration < kClearanceProjectionIterations; ++iteration) {
        glm::vec2 correction(0.0f);
        bool anyOutside = false;
        for (int sampleIndex = 0; sampleIndex < kClearanceSampleDirections; ++sampleIndex) {
            const glm::vec2 direction = clearanceSampleDirection(sampleIndex);
            const float clearanceDistance = supportDistance(profile, direction, inwardDirection);
            const glm::vec3 samplePoint(
                resolved.x + direction.x * clearanceDistance,
                resolved.y,
                resolved.z + direction.y * clearanceDistance
            );
            if (pointInsideAuthoredWalkableSurface(runtime, samplePoint)) {
                continue;
            }

            anyOutside = true;
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
        }

        if (!anyOutside) {
            return resolved;
        }

        const float correctionLength = glm::length(correction);
        if (correctionLength <= kPolygonEpsilon) {
            break;
        }

        const glm::vec2 delta = correction / static_cast<float>(kClearanceSampleDirections);
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
    for (std::size_t cellIndex = 0; cellIndex < runtime.bakedCells.size(); ++cellIndex) {
        const glm::vec3 cellCenter = cellCenter3(runtime, cellIndex);
        considerCandidate(cellCenter);
        considerProjectedCandidate(cellCenter);
    }

    const float nominalClearance = std::max(
        supportDistance(profile, glm::vec2(1.0f, 0.0f), preferredTravelDirection),
        supportDistance(profile, glm::vec2(0.0f, 1.0f), preferredTravelDirection)
    );
    float searchLimit = std::max(nominalClearance * 4.0f, 1.0f);
    if (const std::optional<std::size_t> nearest = findNearestCell(runtime, point); nearest.has_value()) {
        searchLimit = std::max(searchLimit, glm::distance(point, cellCenter3(runtime, *nearest)) + nominalClearance * 2.0f);
    }

    const float radiusStep = std::max(nominalClearance * 0.25f, 0.05f);
    for (float radius = radiusStep; radius <= searchLimit; radius += radiusStep) {
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
        runtime.graph
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
        snapshot.graph
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
        runtime.graph
    });
}

void appendPathCorner(std::vector<glm::vec3>& corners, const glm::vec3& point, float arrivalRadius) {
    if (!corners.empty() && nearlyEqualVec3(corners.back(), point, arrivalRadius)) {
        return;
    }
    corners.push_back(point);
}

render::FrameCounterRecord makeNavigationCounter(const char* name, std::int64_t value) {
    return render::FrameCounterRecord{name, value, "Navigation"};
}

void applyPathResult(NavAgentComponent& agent, const glm::vec3& destination, std::vector<glm::vec3> corners) {
    agent.pathCorners = std::move(corners);
    agent.destination = destination;
    agent.moving = !agent.pathCorners.empty();
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
    const BlockingFootprint& clipper
) {
    if (!polygonHasArea(subject)) {
        return {};
    }
    const auto [subjectMin, subjectMax] = polygonBoundsXZ(subject);
    if (!boundsOverlapXZ(subjectMin, subjectMax, clipper.minXZ, clipper.maxXZ)) {
        return {subject};
    }

    std::vector<std::vector<glm::vec2>> outsidePieces{};
    std::vector<glm::vec2> intersection = subject;
    // Each outside piece satisfies all prior clipper half-planes, so the
    // pieces are convex and non-overlapping while their union is subject - clipper.
    for (std::size_t edgeIndex = 0; edgeIndex < clipper.verticesXZ.size(); ++edgeIndex) {
        const glm::vec2& edgeA = clipper.verticesXZ[edgeIndex];
        const glm::vec2& edgeB = clipper.verticesXZ[(edgeIndex + 1u) % clipper.verticesXZ.size()];

        std::vector<glm::vec2> outside = clipConvexPolygonAgainstHalfPlane(intersection, edgeA, edgeB, false);
        if (polygonHasArea(outside)) {
            outsidePieces.push_back(std::move(outside));
        }
        intersection = clipConvexPolygonAgainstHalfPlane(intersection, edgeA, edgeB, true);
        if (!polygonHasArea(intersection)) {
            break;
        }
    }
    return outsidePieces;
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

std::optional<SharedPortalResult> sharedPortal(const NavPolygon& lhs, const NavPolygon& rhs) {
    auto appendUnique = [](std::vector<glm::vec2>& points, const glm::vec2& point) {
        for (const glm::vec2& existing : points) {
            if (nearlyEqualVec2(existing, point)) {
                return;
            }
        }
        points.push_back(point);
    };

    auto appendSegmentIntersections = [&](std::vector<glm::vec2>& points,
                                          const glm::vec2& a,
                                          const glm::vec2& b,
                                          const glm::vec2& c,
                                          const glm::vec2& d) {
        const glm::vec2 r = b - a;
        const glm::vec2 s = d - c;
        const float rxs = cross2(r, s);
        const float qpxr = cross2(c - a, r);

        if (std::abs(rxs) <= kPolygonEpsilon && std::abs(qpxr) <= kPolygonEpsilon) {
            for (const glm::vec2& candidate : {a, b, c, d}) {
                if (pointOnSegmentXZ(candidate, a, b) && pointOnSegmentXZ(candidate, c, d)) {
                    appendUnique(points, candidate);
                }
            }
            return;
        }
        if (std::abs(rxs) <= kPolygonEpsilon) {
            return;
        }

        const float t = cross2(c - a, s) / rxs;
        const float u = cross2(c - a, r) / rxs;
        if (t < -kPolygonEpsilon || t > 1.0f + kPolygonEpsilon || u < -kPolygonEpsilon || u > 1.0f + kPolygonEpsilon) {
            return;
        }
        appendUnique(points, a + r * t);
    };

    std::vector<glm::vec2> shared{};
    shared.reserve(lhs.verticesXZ.size() + rhs.verticesXZ.size());
    for (const glm::vec2& lhsVertex : lhs.verticesXZ) {
        if (pointInOrOnPolygonXZ(lhsVertex, rhs.verticesXZ)) {
            appendUnique(shared, lhsVertex);
        }
    }
    for (const glm::vec2& rhsVertex : rhs.verticesXZ) {
        if (pointInOrOnPolygonXZ(rhsVertex, lhs.verticesXZ)) {
            appendUnique(shared, rhsVertex);
        }
    }
    for (std::size_t lhsIndex = 0; lhsIndex < lhs.verticesXZ.size(); ++lhsIndex) {
        const glm::vec2& lhsA = lhs.verticesXZ[lhsIndex];
        const glm::vec2& lhsB = lhs.verticesXZ[(lhsIndex + 1u) % lhs.verticesXZ.size()];
        for (std::size_t rhsIndex = 0; rhsIndex < rhs.verticesXZ.size(); ++rhsIndex) {
            const glm::vec2& rhsA = rhs.verticesXZ[rhsIndex];
            const glm::vec2& rhsB = rhs.verticesXZ[(rhsIndex + 1u) % rhs.verticesXZ.size()];
            appendSegmentIntersections(shared, lhsA, lhsB, rhsA, rhsB);
        }
    }
    if (shared.empty()) {
        return std::nullopt;
    }
    if (shared.size() == 1u) {
        return std::nullopt;
    }

    const glm::vec2 origin = shared.front();
    std::optional<glm::vec2> axisPoint{};
    bool allColinear = true;
    for (std::size_t index = 1; index < shared.size(); ++index) {
        if (nearlyEqualVec2(origin, shared[index])) {
            continue;
        }
        axisPoint = shared[index];
        break;
    }
    if (axisPoint.has_value()) {
        const glm::vec2 axis = *axisPoint - origin;
        for (std::size_t index = 1; index < shared.size(); ++index) {
            if (std::abs(cross2(axis, shared[index] - origin)) > kPolygonEpsilon) {
                allColinear = false;
                break;
            }
        }
    }

    if (!allColinear) {
        return std::nullopt;
    }

    glm::vec2 bestA = shared[0];
    glm::vec2 bestB = shared[1];
    float bestDistance = glm::dot(bestB - bestA, bestB - bestA);
    for (std::size_t i = 0; i < shared.size(); ++i) {
        for (std::size_t j = i + 1u; j < shared.size(); ++j) {
            const float distance = glm::dot(shared[j] - shared[i], shared[j] - shared[i]);
            if (distance > bestDistance) {
                bestDistance = distance;
                bestA = shared[i];
                bestB = shared[j];
            }
        }
    }
    if (nearlyEqualVec2(bestA, bestB)) {
        return std::nullopt;
    }
    return SharedPortalResult{bestA, bestB};
}

std::optional<SharedPortalResult> sharedPortal(const NavRuntimeCell& lhs, const NavRuntimeCell& rhs) {
    const NavPolygon lhsPolygon{-1, lhs.elevationY, lhs.verticesXZ};
    const NavPolygon rhsPolygon{-1, rhs.elevationY, rhs.verticesXZ};
    return sharedPortal(lhsPolygon, rhsPolygon);
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
    bool keepLeft
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
        const bool currentInside = currentSide >= -kPolygonEpsilon;
        const bool previousInside = previousSide >= -kPolygonEpsilon;

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

std::vector<std::vector<glm::vec2>> splitConvexPolygonByLine(
    const std::vector<glm::vec2>& polygon,
    const glm::vec2& lineA,
    const glm::vec2& lineB
) {
    if (polygon.size() < 3u || nearlyEqualVec2(lineA, lineB)) {
        return {polygon};
    }

    bool hasPositive = false;
    bool hasNegative = false;
    for (const glm::vec2& vertex : polygon) {
        const float side = cross2(lineB - lineA, vertex - lineA);
        hasPositive |= side > kPolygonEpsilon;
        hasNegative |= side < -kPolygonEpsilon;
    }
    if (!(hasPositive && hasNegative)) {
        return {polygon};
    }

    std::vector<glm::vec2> lhs = clipConvexPolygonAgainstHalfPlane(polygon, lineA, lineB, true);
    std::vector<glm::vec2> rhs = clipConvexPolygonAgainstHalfPlane(polygon, lineA, lineB, false);
    if (!polygonHasArea(lhs) || !polygonHasArea(rhs)) {
        return {polygon};
    }
    return {lhs, rhs};
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

std::vector<std::size_t> authoredPolygonsContainingPoint(const BakeLayerData& layer, const glm::vec2& point) {
    std::vector<std::size_t> containing{};
    for (const AuthoredBakePolygon& polygon : layer.polygons) {
        if (pointInOrOnPolygonXZ(point, polygon.verticesXZ)) {
            containing.push_back(polygon.assetIndex);
        }
    }
    return containing;
}

void appendUniqueIndices(std::vector<std::size_t>& dst, const std::vector<std::size_t>& src) {
    for (std::size_t value : src) {
        if (std::find(dst.begin(), dst.end(), value) == dst.end()) {
            dst.push_back(value);
        }
    }
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

std::optional<SharedPortalResult> sharedBoundaryPortal(
    const NavRuntimeCell& lhs,
    const NavRuntimeCell& rhs
) {
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
            if (overlapEnd - overlapStart <= kPolygonEpsilon) {
                continue;
            }
            return SharedPortalResult{
                lhsA + axis * overlapStart,
                lhsA + axis * overlapEnd
            };
        }
    }
    return std::nullopt;
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

void mergeAdjacentBakeCells(
    std::vector<NavRuntimeCell>& cells,
    std::vector<std::vector<std::size_t>>& cellToPolygonIndices
) {
    mergeAdjacentConvexCellsInternal(cells, &cellToPolygonIndices);
}

std::vector<NavRuntimeCell> bakeLayerRuntimeCells(
    const BakeLayerData& layer,
    std::vector<std::vector<std::size_t>>& outCellToPolygonIndices
) {
    std::vector<std::pair<glm::vec2, glm::vec2>> splitLines{};
    splitLines.reserve(layer.triangles.size() * 3u);
    for (const BakedTriangle& triangle : layer.triangles) {
        for (std::size_t edgeIndex = 0; edgeIndex < 3u; ++edgeIndex) {
            splitLines.emplace_back(
                triangle.verticesXZ[edgeIndex],
                triangle.verticesXZ[(edgeIndex + 1u) % 3u]
            );
        }
    }

    std::unordered_map<std::string, std::size_t> cellIndexByKey{};
    std::vector<NavRuntimeCell> cells{};
    for (const BakedTriangle& triangle : layer.triangles) {
        std::vector<std::vector<glm::vec2>> fragments{
            {
                triangle.verticesXZ[0],
                triangle.verticesXZ[1],
                triangle.verticesXZ[2],
            }
        };

        for (const auto& [lineA, lineB] : splitLines) {
            std::vector<std::vector<glm::vec2>> nextFragments{};
            nextFragments.reserve(fragments.size() * 2u);
            for (const std::vector<glm::vec2>& fragment : fragments) {
                const std::vector<std::vector<glm::vec2>> split = splitConvexPolygonByLine(fragment, lineA, lineB);
                nextFragments.insert(nextFragments.end(), split.begin(), split.end());
            }
            fragments = std::move(nextFragments);
        }

        for (std::vector<glm::vec2>& fragment : fragments) {
            fragment = normalizePolygonVertices(fragment);
            if (!polygonHasArea(fragment)) {
                continue;
            }

            const glm::vec2 centroid = polygonCentroidXZ(fragment);
            std::vector<std::size_t> memberships = authoredPolygonsContainingPoint(layer, centroid);
            if (memberships.empty()) {
                continue;
            }
            std::sort(memberships.begin(), memberships.end());

            const std::string key = canonicalPolygonKey(fragment);
            if (key.empty()) {
                continue;
            }

            const auto [it, inserted] = cellIndexByKey.emplace(key, cells.size());
            if (inserted) {
                cells.push_back(NavRuntimeCell{layer.elevationY, fragment});
                outCellToPolygonIndices.push_back(std::move(memberships));
            } else {
                appendUniqueIndices(outCellToPolygonIndices[it->second], memberships);
                std::sort(outCellToPolygonIndices[it->second].begin(), outCellToPolygonIndices[it->second].end());
            }
        }
    }

    mergeAdjacentBakeCells(cells, outCellToPolygonIndices);
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
            if (polygonHasArea(clipConvexPolygons(lhs.verticesXZ, rhs.verticesXZ))) {
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
        if (std::abs(point.y - cell.elevationY) > 1.0f) {
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
    for (std::size_t index = 0; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        const glm::vec2 center = index < runtime.bakedCellCenters.size()
            ? runtime.bakedCellCenters[index]
            : polygonCentroidXZ(cell.verticesXZ);
        const float planarDistance = glm::distance(center, glm::vec2(point.x, point.z));
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
        if (std::abs(point.y - cell.elevationY) > 1.0f) {
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
        if (std::abs(point.y - cell.elevationY) > 1.0f) {
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
    const glm::vec3& to
) {
    if (std::abs(from.y - to.y) > kLayerGroupingEpsilon) {
        return false;
    }

    const glm::vec2 segmentStart(from.x, from.z);
    const glm::vec2 segmentEnd(to.x, to.z);
    const glm::vec2 segmentDelta = segmentEnd - segmentStart;
    if (glm::dot(segmentDelta, segmentDelta) <= kPlaneEpsilon * kPlaneEpsilon) {
        return pointInsideAuthoredWalkableSurface(runtime, from);
    }

    const glm::vec2 segmentMin = glm::min(segmentStart, segmentEnd);
    const glm::vec2 segmentMax = glm::max(segmentStart, segmentEnd);
    std::vector<std::pair<float, float>> coveredIntervals{};
    coveredIntervals.reserve(runtime.bakedCells.size());
    for (std::size_t cellIndex = 0u; cellIndex < runtime.bakedCells.size(); ++cellIndex) {
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        if (std::abs(from.y - cell.elevationY) > 1.0f) {
            continue;
        }
        if (cellIndex < runtime.bakedCellMinXZ.size() && cellIndex < runtime.bakedCellMaxXZ.size() &&
            !boundsOverlapXZ(
                segmentMin,
                segmentMax,
                runtime.bakedCellMinXZ[cellIndex],
                runtime.bakedCellMaxXZ[cellIndex])) {
            continue;
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
        if (std::abs(point.y - cell.elevationY) > 1.0f) {
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
    for (std::size_t index = 0; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        const glm::vec2 center = index < runtime.bakedCellCenters.size()
            ? runtime.bakedCellCenters[index]
            : polygonCentroidXZ(cell.verticesXZ);
        const float planarDistance = glm::distance(center, glm::vec2(point.x, point.z));
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
        if (std::abs(point.y - cell.elevationY) > 1.0f) {
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
        if (std::abs(point.y - cell.elevationY) > 1.0f) {
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
    for (int sampleIndex = 0; sampleIndex < kSegmentClearanceSampleDirections; ++sampleIndex) {
        const glm::vec2 direction = clearanceSampleDirection(
            sampleIndex,
            kSegmentClearanceSampleDirections
        );
        const float distance = supportDistance(profile, direction, travelDirection);
        const glm::vec3 offset(direction.x * distance, 0.0f, direction.y * distance);
        if (!segmentInsideAuthoredWalkableSurface(runtime, from + offset, to + offset)) {
            return false;
        }
    }
    return true;
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

std::optional<std::vector<glm::vec3>> solveVisibilityPath(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const glm::vec3& destination,
    const AgentClearanceProfile& profile,
    const std::vector<std::size_t>& startCells
) {
    if (std::abs(start.y - destination.y) > kLayerGroupingEpsilon) {
        return std::nullopt;
    }
    std::vector<glm::vec3> nodes{};
    const auto addNode = [&](const glm::vec3& point) {
        if (std::abs(point.y - start.y) > kLayerGroupingEpsilon) {
            return;
        }
        if (!pointInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                point,
                profile,
                travelDirectionForSegment(start, destination))) {
            return;
        }
        const auto existing = std::find_if(nodes.begin(), nodes.end(), [&](const glm::vec3& node) {
            return nearlyEqualVec3(node, point);
        });
        if (existing == nodes.end()) {
            nodes.push_back(point);
        }
    };

    // Enumerate the same-layer component used by the visibility search.
    std::vector<std::uint8_t> visibilityCells(runtime.bakedCells.size(), 0u);
    std::vector<std::size_t> pendingCells{};
    for (std::size_t startCell : startCells) {
        if (startCell >= runtime.bakedCells.size() || visibilityCells[startCell] != 0u) {
            continue;
        }
        visibilityCells[startCell] = 1u;
        pendingCells.push_back(startCell);
    }
    while (!pendingCells.empty()) {
        const std::size_t cellIndex = pendingCells.back();
        pendingCells.pop_back();
        for (const NavGraphEdge& edge : runtime.graph[cellIndex]) {
            if (edge.viaLink ||
                edge.targetCellIndex >= runtime.bakedCells.size() ||
                visibilityCells[edge.targetCellIndex] != 0u ||
                std::abs(runtime.bakedCells[edge.targetCellIndex].elevationY - start.y) > kLayerGroupingEpsilon) {
                continue;
            }
            if (!shrinkPortal(
                    edge.portalA,
                    edge.portalB,
                    profile,
                    runtime.bakedCellCenters[edge.targetCellIndex] - runtime.bakedCellCenters[cellIndex]
                ).has_value() &&
                !cellsShareAuthoredPolygon(runtime, cellIndex, edge.targetCellIndex)) {
                continue;
            }
            visibilityCells[edge.targetCellIndex] = 1u;
            pendingCells.push_back(edge.targetCellIndex);
        }
    }

    std::unordered_map<QuantizedLayerPoint, bool, QuantizedLayerPointHash> boundaryByPoint{};
    const auto touchesWalkableBoundary = [&](const glm::vec2& point,
                                             std::size_t cellIndex,
                                             std::size_t vertexIndex) {
        if (cellIndex < runtime.bakedCellBoundaryVertices.size() &&
            vertexIndex < runtime.bakedCellBoundaryVertices[cellIndex].size()) {
            return runtime.bakedCellBoundaryVertices[cellIndex][vertexIndex] != 0u;
        }

        const QuantizedLayerPoint key = quantizeLayerPoint(point, start.y);
        if (const auto found = boundaryByPoint.find(key); found != boundaryByPoint.end()) {
            return found->second;
        }

        constexpr float kBoundaryProbeDistance = kPolygonEpsilon * 8.0f;
        bool boundary = false;
        for (int directionIndex = 0; directionIndex < kSegmentClearanceSampleDirections; ++directionIndex) {
            const glm::vec2 probe = point +
                clearanceSampleDirection(directionIndex, kSegmentClearanceSampleDirections) * kBoundaryProbeDistance;
            if (!pointInsideAuthoredWalkableSurface(runtime, glm::vec3(probe.x, start.y, probe.y))) {
                boundary = true;
                break;
            }
        }
        boundaryByPoint.emplace(key, boundary);
        return boundary;
    };

    addNode(start);
    addNode(destination);
    if (nodes.size() < 2u) {
        return std::nullopt;
    }

    for (std::size_t cellIndex = 0; cellIndex < runtime.bakedCells.size(); ++cellIndex) {
        if (visibilityCells[cellIndex] == 0u) {
            continue;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        if (std::abs(cell.elevationY - start.y) > kLayerGroupingEpsilon) {
            continue;
        }
        for (std::size_t vertexIndex = 0u; vertexIndex < cell.verticesXZ.size(); ++vertexIndex) {
            const glm::vec2& vertex = cell.verticesXZ[vertexIndex];
            if (touchesWalkableBoundary(vertex, cellIndex, vertexIndex)) {
                addNode(glm::vec3(vertex.x, cell.elevationY, vertex.y));
            }
        }
    }

    for (std::size_t fromCell = 0u; !profile.empty() && fromCell < runtime.bakedCells.size(); ++fromCell) {
        if (visibilityCells[fromCell] == 0u) {
            continue;
        }
        for (const NavGraphEdge& edge : runtime.graph[fromCell]) {
            const std::size_t toCell = edge.targetCellIndex;
            if (edge.viaLink ||
                toCell >= runtime.bakedCells.size() ||
                fromCell >= toCell ||
                visibilityCells[toCell] == 0u) {
                continue;
            }

            std::optional<SharedPortalResult> portal = shrinkPortal(
                edge.portalA,
                edge.portalB,
                profile,
                runtime.bakedCellCenters[toCell] - runtime.bakedCellCenters[fromCell]
            );
            if (!portal.has_value() && cellsShareAuthoredPolygon(runtime, fromCell, toCell)) {
                portal = SharedPortalResult{edge.portalA, edge.portalB};
            }
            if (!portal.has_value()) {
                continue;
            }
            addNode(glm::vec3(portal->a.x, runtime.bakedCells[fromCell].elevationY, portal->a.y));
            addNode(glm::vec3(portal->b.x, runtime.bakedCells[fromCell].elevationY, portal->b.y));
        }
    }

    const std::size_t nodeCount = nodes.size();
    std::unordered_map<std::uint64_t, bool> visibilityCache{};
    visibilityCache.reserve(nodeCount * 4u);
    const auto canTraverse = [&](std::size_t lhs, std::size_t rhs) {
        const std::size_t lower = std::min(lhs, rhs);
        const std::size_t upper = std::max(lhs, rhs);
        const std::uint64_t key =
            (static_cast<std::uint64_t>(lower) << 32u) |
            static_cast<std::uint64_t>(upper);
        if (const auto found = visibilityCache.find(key); found != visibilityCache.end()) {
            return found->second;
        }
        const bool visible = segmentInsideAuthoredWalkableSurfaceWithClearance(
            runtime,
            nodes[lhs],
            nodes[rhs],
            profile
        );
        visibilityCache.emplace(key, visible);
        return visible;
    };

    struct QueueItem {
        float fScore{0.0f};
        float gScore{0.0f};
        std::size_t nodeIndex{0u};

        bool operator<(const QueueItem& other) const {
            return fScore > other.fScore;
        }
    };

    std::vector<float> distances(nodeCount, std::numeric_limits<float>::max());
    std::vector<int> parents(nodeCount, -1);
    std::vector<std::uint8_t> closed(nodeCount, 0u);
    std::priority_queue<QueueItem> open{};
    distances[0] = 0.0f;
    open.push(QueueItem{glm::distance(nodes[0], nodes[1]), 0.0f, 0u});
    while (!open.empty()) {
        const QueueItem current = open.top();
        open.pop();
        if (current.gScore > distances[current.nodeIndex] + kPolygonEpsilon || closed[current.nodeIndex] != 0u) {
            continue;
        }
        closed[current.nodeIndex] = 1u;
        if (current.nodeIndex == 1u) {
            break;
        }

        // The Euclidean heuristic becomes the exact remaining cost when the
        // destination is visible, so no queued route can produce a shorter path.
        if (canTraverse(current.nodeIndex, 1u)) {
            parents[1] = static_cast<int>(current.nodeIndex);
            distances[1] = distances[current.nodeIndex] + glm::distance(nodes[current.nodeIndex], nodes[1]);
            break;
        }

        for (std::size_t nextNode = 1u; nextNode < nodeCount; ++nextNode) {
            if (nextNode == current.nodeIndex || closed[nextNode] != 0u) {
                continue;
            }
            const float edgeDistance = glm::distance(nodes[current.nodeIndex], nodes[nextNode]);
            const float candidate = distances[current.nodeIndex] + edgeDistance;
            if (candidate >= distances[nextNode]) {
                continue;
            }
            if (!canTraverse(current.nodeIndex, nextNode)) {
                continue;
            }
            distances[nextNode] = candidate;
            parents[nextNode] = static_cast<int>(current.nodeIndex);
            open.push(QueueItem{candidate + glm::distance(nodes[nextNode], nodes[1]), candidate, nextNode});
        }
    }
    if (parents[1] < 0 && !nearlyEqualVec3(start, destination)) {
        return std::nullopt;
    }

    std::vector<glm::vec3> reversed{};
    for (std::size_t nodeIndex = 1u; nodeIndex != 0u;) {
        reversed.push_back(nodes[nodeIndex]);
        const int parent = parents[nodeIndex];
        if (parent < 0) {
            return std::nullopt;
        }
        nodeIndex = static_cast<std::size_t>(parent);
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

// --- Pathfinding pipeline ---

std::optional<ResolvedPathEndpoints> resolvePathEndpoints(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const glm::vec3& destination,
    const AgentClearanceProfile& profile
) {
    ResolvedPathEndpoints endpoints{};
    if (!profile.empty()) {
        const glm::vec2 travelDir = travelDirectionForSegment(start, destination, glm::vec2(0.0f, 1.0f));
        endpoints.resolvedStart = resolvePointWithClearance(runtime, start, profile, travelDir).value_or(start);

        // Destination: prefer approach-line projection for natural corner clearance.
        // Walk from click along reverse-approach direction until a safe position is found,
        // then binary-search back to find the nearest safe point along that line.
        const glm::vec2 approachDir = -travelDir;
        if (pointInsideAuthoredWalkableSurfaceWithClearance(runtime, destination, profile, approachDir)) {
            endpoints.resolvedDestination = destination;
        } else {
            const float maxSearch = glm::distance(glm::vec2(start.x, start.z), glm::vec2(destination.x, destination.z));
            const float nominalClearance = std::max(
                supportDistance(profile, glm::vec2(1.0f, 0.0f), approachDir),
                supportDistance(profile, glm::vec2(0.0f, 1.0f), approachDir)
            );
            const float step = std::max(nominalClearance * 0.25f, 0.05f);
            glm::vec3 safeFar{};
            bool foundSafe = false;
            for (float t = step; t <= maxSearch; t += step) {
                safeFar = glm::vec3(
                    destination.x + approachDir.x * t,
                    destination.y,
                    destination.z + approachDir.y * t
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
                    const glm::vec3 mid = destination + (safeFar - destination) * midT;
                    if (pointInsideAuthoredWalkableSurface(runtime, mid) &&
                        pointInsideAuthoredWalkableSurfaceWithClearance(runtime, mid, profile, approachDir)) {
                        safeT = midT;
                    } else {
                        unsafeT = midT;
                    }
                }
                endpoints.resolvedDestination = destination + (safeFar - destination) * safeT;
            } else {
                endpoints.resolvedDestination = resolvePointWithClearance(runtime, destination, profile, -travelDir).value_or(destination);
            }
        }
    } else {
        endpoints.resolvedStart = start;
        endpoints.resolvedDestination = destination;
    }
    endpoints.rawStartCells = findContainingCells(runtime, start);
    endpoints.rawTargetCells = findContainingCells(runtime, destination);
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

// Polyanya-style interval A*: a node represents every visible crossing point
// on one portal, rooted at the last real turn in the geometric path.
constexpr std::size_t kInvalidIntervalNode = std::numeric_limits<std::size_t>::max();

struct IntervalTransition {
    std::array<glm::vec3, 3u> points{};
    std::uint8_t count{0u};
};

struct IntervalSearchNode {
    glm::vec3 root{0.0f};
    glm::vec2 entryA{0.0f};
    glm::vec2 entryB{0.0f};
    glm::vec2 entryPortalA{0.0f};
    glm::vec2 entryPortalB{0.0f};
    std::size_t cellIndex{0u};
    std::size_t previousCellIndex{kInvalidIntervalNode};
    std::size_t parentIndex{kInvalidIntervalNode};
    bool hasEntryInterval{false};
    float gScore{0.0f};
    float fScore{0.0f};
    IntervalTransition transition{};
};

struct IntervalStateKey {
    QuantizedVec2 root{};
    QuantizedVec2 entryPortalA{};
    QuantizedVec2 entryPortalB{};
    std::size_t cellIndex{0u};
    std::size_t previousCellIndex{kInvalidIntervalNode};
    bool hasEntryInterval{false};

    friend bool operator==(const IntervalStateKey&, const IntervalStateKey&) = default;
};

struct IntervalStateKeyHash {
    std::size_t operator()(const IntervalStateKey& key) const noexcept {
        QuantizedVec2Hash pointHash{};
        std::size_t result = pointHash(key.root);
        result ^= pointHash(key.entryPortalA) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
        result ^= pointHash(key.entryPortalB) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
        result ^= std::hash<std::size_t>{}(key.cellIndex) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
        result ^= std::hash<std::size_t>{}(key.previousCellIndex) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
        result ^= static_cast<std::size_t>(key.hasEntryInterval);
        return result;
    }
};

IntervalStateKey makeIntervalStateKey(const IntervalSearchNode& node) {
    IntervalStateKey key{};
    key.root = quantizeVec2(glm::vec2(node.root.x, node.root.z));
    key.cellIndex = node.cellIndex;
    key.previousCellIndex = node.previousCellIndex;
    key.hasEntryInterval = node.hasEntryInterval;
    if (node.hasEntryInterval) {
        key.entryPortalA = quantizeVec2(node.entryPortalA);
        key.entryPortalB = quantizeVec2(node.entryPortalB);
        if (key.entryPortalB < key.entryPortalA) {
            std::swap(key.entryPortalA, key.entryPortalB);
        }
    }
    return key;
}

IntervalTransition makeIntervalTransition(
    const glm::vec3& first,
    const std::optional<glm::vec3>& second = std::nullopt,
    const std::optional<glm::vec3>& third = std::nullopt
) {
    IntervalTransition transition{};
    transition.points[transition.count++] = first;
    if (second.has_value()) {
        transition.points[transition.count++] = *second;
    }
    if (third.has_value()) {
        transition.points[transition.count++] = *third;
    }
    return transition;
}

glm::vec2 intervalProbeInsideCell(
    const NavigationSolveView& runtime,
    const IntervalSearchNode& node
) {
    const glm::vec2 midpoint = (node.entryA + node.entryB) * 0.5f;
    glm::vec2 inward = runtime.bakedCellCenters[node.cellIndex] - midpoint;
    const float inwardLength = glm::length(inward);
    if (inwardLength <= kPlaneEpsilon) {
        const glm::vec2 edge = node.entryB - node.entryA;
        inward = normalizeOrFallback(glm::vec2(-edge.y, edge.x));
    } else {
        inward /= inwardLength;
    }
    return midpoint + inward * std::max(kPolygonEpsilon * 8.0f, 0.001f);
}

bool pointInsideIntervalCone(
    const NavigationSolveView& runtime,
    const IntervalSearchNode& node,
    const glm::vec2& point
) {
    if (!node.hasEntryInterval || pointOnSegmentXZ(
            glm::vec2(node.root.x, node.root.z), node.entryA, node.entryB)) {
        return true;
    }

    const glm::vec2 root(node.root.x, node.root.z);
    const glm::vec2 probe = intervalProbeInsideCell(runtime, node);
    for (const glm::vec2& boundary : {node.entryA, node.entryB}) {
        const glm::vec2 ray = boundary - root;
        if (glm::dot(ray, ray) <= kPlaneEpsilon * kPlaneEpsilon) {
            continue;
        }
        const float probeSide = cross2(ray, probe - root);
        if (std::abs(probeSide) <= kPlaneEpsilon) {
            return false;
        }
        const float pointSide = cross2(ray, point - root);
        if ((probeSide > 0.0f ? pointSide : -pointSide) < -kPolygonEpsilon) {
            return false;
        }
    }
    return true;
}

std::optional<SharedPortalResult> clipPortalToIntervalCone(
    const NavigationSolveView& runtime,
    const IntervalSearchNode& node,
    const SharedPortalResult& portal
) {
    if (!node.hasEntryInterval || pointOnSegmentXZ(
            glm::vec2(node.root.x, node.root.z), node.entryA, node.entryB)) {
        return portal;
    }

    const glm::vec2 root(node.root.x, node.root.z);
    const glm::vec2 probe = intervalProbeInsideCell(runtime, node);
    const glm::vec2 delta = portal.b - portal.a;
    float minimumT = 0.0f;
    float maximumT = 1.0f;
    for (const glm::vec2& boundary : {node.entryA, node.entryB}) {
        const glm::vec2 ray = boundary - root;
        if (glm::dot(ray, ray) <= kPlaneEpsilon * kPlaneEpsilon) {
            continue;
        }
        const float probeSide = cross2(ray, probe - root);
        if (std::abs(probeSide) <= kPlaneEpsilon) {
            return std::nullopt;
        }
        const float sign = probeSide > 0.0f ? 1.0f : -1.0f;
        const float sideAtStart = sign * cross2(ray, portal.a - root);
        const float sideDelta = sign * cross2(ray, delta);
        if (std::abs(sideDelta) <= kPlaneEpsilon) {
            if (sideAtStart < -kPolygonEpsilon) {
                return std::nullopt;
            }
            continue;
        }

        const float boundaryT = (-kPolygonEpsilon - sideAtStart) / sideDelta;
        if (sideDelta > 0.0f) {
            minimumT = std::max(minimumT, boundaryT);
        } else {
            maximumT = std::min(maximumT, boundaryT);
        }
        if (minimumT > maximumT + kPolygonEpsilon) {
            return std::nullopt;
        }
    }

    minimumT = std::clamp(minimumT, 0.0f, 1.0f);
    maximumT = std::clamp(maximumT, 0.0f, 1.0f);
    if (minimumT > maximumT + kPolygonEpsilon) {
        return std::nullopt;
    }
    return SharedPortalResult{
        portal.a + delta * minimumT,
        portal.a + delta * maximumT
    };
}

bool portalsNearlyEqual(const SharedPortalResult& lhs, const SharedPortalResult& rhs) {
    return (nearlyEqualVec2(lhs.a, rhs.a) && nearlyEqualVec2(lhs.b, rhs.b)) ||
        (nearlyEqualVec2(lhs.a, rhs.b) && nearlyEqualVec2(lhs.b, rhs.a));
}

std::optional<SharedPortalResult> intervalPortalForEdge(
    const NavigationSolveView& runtime,
    const IntervalSearchNode& node,
    const NavGraphEdge& edge,
    const AgentClearanceProfile& profile
) {
    if (edge.viaLink) {
        return std::nullopt;
    }
    if (profile.empty() || cellsShareAuthoredPolygon(runtime, node.cellIndex, edge.targetCellIndex)) {
        return SharedPortalResult{edge.portalA, edge.portalB};
    }

    const glm::vec2 midpoint = (edge.portalA + edge.portalB) * 0.5f;
    const glm::vec2 root(node.root.x, node.root.z);
    const glm::vec2 edgeDirection = edge.portalB - edge.portalA;
    const glm::vec2 fallbackDirection = normalizeOrFallback(glm::vec2(-edgeDirection.y, edgeDirection.x));
    return shrinkPortal(
        edge.portalA,
        edge.portalB,
        profile,
        normalizeOrFallback(midpoint - root, fallbackDirection)
    );
}

struct IntervalPointReach {
    float cost{0.0f};
    std::optional<glm::vec3> turn{};
};

std::vector<IntervalPointReach> intervalPointReaches(
    const NavigationSolveView& runtime,
    const IntervalSearchNode& node,
    const glm::vec3& point,
    const AgentClearanceProfile& profile,
    bool requireSegmentValidation
) {
    std::vector<IntervalPointReach> reaches{};
    reaches.reserve(2u);
    const bool direct = !node.hasEntryInterval ||
        pointInsideIntervalCone(runtime, node, glm::vec2(point.x, point.z));
    if (direct && (!requireSegmentValidation ||
        segmentInsideAuthoredWalkableSurfaceWithClearance(runtime, node.root, point, profile))) {
        reaches.push_back(IntervalPointReach{
            node.gScore + glm::distance(node.root, point),
            std::nullopt
        });
        return reaches;
    }
    if (!node.hasEntryInterval) {
        return reaches;
    }

    const float elevation = runtime.bakedCells[node.cellIndex].elevationY;
    for (const glm::vec2& endpoint : {node.entryA, node.entryB}) {
        const glm::vec3 turn(endpoint.x, elevation, endpoint.y);
        if (!reaches.empty() && reaches.back().turn.has_value() &&
            nearlyEqualVec3(*reaches.back().turn, turn)) {
            continue;
        }
        if (requireSegmentValidation &&
            (!segmentInsideAuthoredWalkableSurfaceWithClearance(runtime, node.root, turn, profile) ||
             !segmentInsideAuthoredWalkableSurfaceWithClearance(runtime, turn, point, profile))) {
            continue;
        }
        reaches.push_back(IntervalPointReach{
            node.gScore + glm::distance(node.root, turn) + glm::distance(turn, point),
            nearlyEqualVec3(node.root, turn) ? std::optional<glm::vec3>{} : std::optional<glm::vec3>{turn}
        });
    }
    return reaches;
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

bool intervalPathIsValid(
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

std::optional<std::vector<glm::vec3>> solveIntervalPath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const AgentClearanceProfile& profile
) {
    if (runtime.bakedCells.empty()) {
        return std::nullopt;
    }

    struct QueueItem {
        float fScore{0.0f};
        float gScore{0.0f};
        std::size_t nodeIndex{0u};

        bool operator<(const QueueItem& other) const {
            if (fScore != other.fScore) {
                return fScore > other.fScore;
            }
            return gScore < other.gScore;
        }
    };
    struct GoalResult {
        float cost{std::numeric_limits<float>::max()};
        std::size_t parentIndex{kInvalidIntervalNode};
        IntervalTransition transition{};
    };
    struct DominanceRecord {
        float minimumT{0.0f};
        float maximumT{1.0f};
        float gScore{0.0f};
        std::size_t nodeIndex{0u};
    };

    std::vector<IntervalSearchNode> nodes{};
    nodes.reserve(std::max<std::size_t>(runtime.bakedCells.size() * 8u, 64u));
    std::vector<QueueItem> queueStorage{};
    queueStorage.reserve(nodes.capacity());
    std::priority_queue<QueueItem, std::vector<QueueItem>> open(
        std::less<QueueItem>{},
        std::move(queueStorage)
    );
    std::unordered_map<IntervalStateKey, std::vector<DominanceRecord>, IntervalStateKeyHash> frontierByState{};
    frontierByState.reserve(nodes.capacity());
    std::unordered_map<QuantizedLayerPoint, bool, QuantizedLayerPointHash> boundaryPointCache{};
    boundaryPointCache.reserve(runtime.bakedCells.size() * 2u);
    const auto canTurnAtEndpoint = [&](const glm::vec3& point, std::size_t cellIndex) {
        if (!profile.empty()) {
            return true;
        }
        if (cellIndex < runtime.bakedCells.size() &&
            cellIndex < runtime.bakedCellBoundaryVertices.size()) {
            const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
            const std::vector<std::uint8_t>& boundaryVertices =
                runtime.bakedCellBoundaryVertices[cellIndex];
            for (std::size_t vertexIndex = 0u;
                 vertexIndex < cell.verticesXZ.size() && vertexIndex < boundaryVertices.size();
                 ++vertexIndex) {
                if (nearlyEqualVec2(cell.verticesXZ[vertexIndex], glm::vec2(point.x, point.z))) {
                    return boundaryVertices[vertexIndex] != 0u;
                }
            }
        }

        const QuantizedLayerPoint key = quantizeLayerPoint(glm::vec2(point.x, point.z), point.y);
        if (const auto found = boundaryPointCache.find(key); found != boundaryPointCache.end()) {
            return found->second;
        }

        constexpr float kBoundaryProbeDistance = kPolygonEpsilon * 8.0f;
        bool boundary = false;
        for (int directionIndex = 0; directionIndex < kSegmentClearanceSampleDirections; ++directionIndex) {
            const glm::vec2 direction = clearanceSampleDirection(
                directionIndex,
                kSegmentClearanceSampleDirections
            );
            if (!pointInsideAuthoredWalkableSurface(
                    runtime,
                    glm::vec3(
                        point.x + direction.x * kBoundaryProbeDistance,
                        point.y,
                        point.z + direction.y * kBoundaryProbeDistance))) {
                boundary = true;
                break;
            }
        }
        boundaryPointCache.emplace(key, boundary);
        return boundary;
    };

    const auto pushNode = [&](IntervalSearchNode candidate) {
        candidate.fScore = candidate.gScore + glm::distance(candidate.root, endpoints.resolvedDestination);
        const IntervalStateKey key = makeIntervalStateKey(candidate);
        float minimumT = 0.0f;
        float maximumT = 1.0f;
        if (candidate.hasEntryInterval) {
            const glm::vec2 portalDelta = candidate.entryPortalB - candidate.entryPortalA;
            const float portalLengthSquared = glm::dot(portalDelta, portalDelta);
            if (portalLengthSquared <= kPlaneEpsilon * kPlaneEpsilon) {
                return;
            }
            minimumT = glm::dot(candidate.entryA - candidate.entryPortalA, portalDelta) / portalLengthSquared;
            maximumT = glm::dot(candidate.entryB - candidate.entryPortalA, portalDelta) / portalLengthSquared;
            if (minimumT > maximumT) {
                std::swap(minimumT, maximumT);
            }
            minimumT = std::clamp(minimumT, 0.0f, 1.0f);
            maximumT = std::clamp(maximumT, 0.0f, 1.0f);
        }

        std::vector<DominanceRecord>& frontier = frontierByState[key];
        if (std::any_of(frontier.begin(), frontier.end(), [&](const DominanceRecord& existing) {
                return existing.gScore <= candidate.gScore + kPolygonEpsilon &&
                    existing.minimumT <= minimumT + kPolygonEpsilon &&
                    existing.maximumT >= maximumT - kPolygonEpsilon;
            })) {
            return;
        }
        const std::size_t nodeIndex = nodes.size();
        frontier.erase(std::remove_if(frontier.begin(), frontier.end(), [&](const DominanceRecord& existing) {
            return candidate.gScore <= existing.gScore + kPolygonEpsilon &&
                minimumT <= existing.minimumT + kPolygonEpsilon &&
                maximumT >= existing.maximumT - kPolygonEpsilon;
        }), frontier.end());
        frontier.push_back(DominanceRecord{minimumT, maximumT, candidate.gScore, nodeIndex});
        nodes.push_back(std::move(candidate));
        open.push(QueueItem{nodes.back().fScore, nodes.back().gScore, nodeIndex});
    };

    for (std::size_t startCell : endpoints.startCells) {
        if (startCell >= runtime.bakedCells.size()) {
            continue;
        }
        pushNode(IntervalSearchNode{
            endpoints.resolvedStart,
            glm::vec2(0.0f),
            glm::vec2(0.0f),
            glm::vec2(0.0f),
            glm::vec2(0.0f),
            startCell,
            kInvalidIntervalNode,
            kInvalidIntervalNode,
            false,
            0.0f,
            0.0f,
            IntervalTransition{}
        });
    }
    if (open.empty()) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> targetCells(runtime.bakedCells.size(), 0u);
    for (std::size_t targetCell : endpoints.targetCells) {
        if (targetCell < targetCells.size()) {
            targetCells[targetCell] = 1u;
        }
    }

    GoalResult goal{};
    while (!open.empty()) {
        const QueueItem currentItem = open.top();
        open.pop();
        if (currentItem.fScore >= goal.cost - kPolygonEpsilon) {
            break;
        }
        if (currentItem.nodeIndex >= nodes.size()) {
            continue;
        }
        const IntervalSearchNode current = nodes[currentItem.nodeIndex];
        const IntervalStateKey currentKey = makeIntervalStateKey(current);
        const auto frontier = frontierByState.find(currentKey);
        if (frontier == frontierByState.end() ||
            std::none_of(frontier->second.begin(), frontier->second.end(), [&](const DominanceRecord& record) {
                return record.nodeIndex == currentItem.nodeIndex;
            })) {
            continue;
        }

        if (current.cellIndex < targetCells.size() && targetCells[current.cellIndex] != 0u) {
            for (const IntervalPointReach& reach : intervalPointReaches(
                     runtime,
                     current,
                     endpoints.resolvedDestination,
                     profile,
                     !profile.empty())) {
                if (reach.cost >= goal.cost) {
                    continue;
                }
                goal.cost = reach.cost;
                goal.parentIndex = currentItem.nodeIndex;
                goal.transition = reach.turn.has_value()
                    ? makeIntervalTransition(*reach.turn, endpoints.resolvedDestination)
                    : makeIntervalTransition(endpoints.resolvedDestination);
            }
        }

        if (current.cellIndex >= runtime.graph.size()) {
            continue;
        }
        for (const NavGraphEdge& edge : runtime.graph[current.cellIndex]) {
            if (edge.targetCellIndex >= runtime.bakedCells.size()) {
                continue;
            }
            if (edge.viaLink) {
                for (const IntervalPointReach& reach : intervalPointReaches(
                         runtime,
                         current,
                         edge.linkStartPoint,
                         profile,
                         true)) {
                    IntervalTransition transition{};
                    if (reach.turn.has_value()) {
                        transition = makeIntervalTransition(
                            *reach.turn,
                            edge.linkStartPoint,
                            edge.linkEndPoint
                        );
                    } else {
                        transition = makeIntervalTransition(edge.linkStartPoint, edge.linkEndPoint);
                    }
                    pushNode(IntervalSearchNode{
                        edge.linkEndPoint,
                        glm::vec2(0.0f),
                        glm::vec2(0.0f),
                        glm::vec2(0.0f),
                        glm::vec2(0.0f),
                        edge.targetCellIndex,
                        kInvalidIntervalNode,
                        currentItem.nodeIndex,
                        false,
                        reach.cost + glm::distance(edge.linkStartPoint, edge.linkEndPoint),
                        0.0f,
                        transition
                    });
                }
                continue;
            }
            if (edge.targetCellIndex == current.previousCellIndex) {
                continue;
            }

            const std::optional<SharedPortalResult> safePortal =
                intervalPortalForEdge(runtime, current, edge, profile);
            if (!safePortal.has_value()) {
                continue;
            }
            const std::optional<SharedPortalResult> observable =
                clipPortalToIntervalCone(runtime, current, *safePortal);
            if (observable.has_value()) {
                pushNode(IntervalSearchNode{
                    current.root,
                    observable->a,
                    observable->b,
                    safePortal->a,
                    safePortal->b,
                    edge.targetCellIndex,
                    current.cellIndex,
                    currentItem.nodeIndex,
                    true,
                    current.gScore,
                    0.0f,
                    IntervalTransition{}
                });
            }

            if (!current.hasEntryInterval ||
                (observable.has_value() && portalsNearlyEqual(*observable, *safePortal))) {
                continue;
            }
            const float elevation = runtime.bakedCells[current.cellIndex].elevationY;
            for (const glm::vec2& endpoint : {current.entryA, current.entryB}) {
                if (!nearlyEqualVec2(endpoint, current.entryPortalA) &&
                    !nearlyEqualVec2(endpoint, current.entryPortalB)) {
                    continue;
                }
                const glm::vec3 turn(endpoint.x, elevation, endpoint.y);
                if (!canTurnAtEndpoint(turn, current.cellIndex)) {
                    continue;
                }
                if (!profile.empty() &&
                    !segmentInsideAuthoredWalkableSurfaceWithClearance(runtime, current.root, turn, profile)) {
                    continue;
                }
                const bool rootChanges = !nearlyEqualVec3(current.root, turn);
                pushNode(IntervalSearchNode{
                    turn,
                    safePortal->a,
                    safePortal->b,
                    safePortal->a,
                    safePortal->b,
                    edge.targetCellIndex,
                    current.cellIndex,
                    currentItem.nodeIndex,
                    true,
                    current.gScore + glm::distance(current.root, turn),
                    0.0f,
                    rootChanges ? makeIntervalTransition(turn) : IntervalTransition{}
                });
            }
        }
    }

    if (goal.parentIndex == kInvalidIntervalNode) {
        return std::nullopt;
    }

    std::vector<std::size_t> lineage{};
    for (std::size_t nodeIndex = goal.parentIndex;
         nodeIndex != kInvalidIntervalNode;
         nodeIndex = nodes[nodeIndex].parentIndex) {
        lineage.push_back(nodeIndex);
    }
    std::reverse(lineage.begin(), lineage.end());

    std::vector<glm::vec3> corners{};
    corners.reserve(lineage.size() + 2u);
    for (std::size_t nodeIndex : lineage) {
        const IntervalTransition& transition = nodes[nodeIndex].transition;
        for (std::uint8_t pointIndex = 0u; pointIndex < transition.count; ++pointIndex) {
            appendPathCorner(corners, transition.points[pointIndex], kPolygonEpsilon);
        }
    }
    for (std::uint8_t pointIndex = 0u; pointIndex < goal.transition.count; ++pointIndex) {
        appendPathCorner(corners, goal.transition.points[pointIndex], kPolygonEpsilon);
    }
    return corners;
}

std::optional<SolvedPath> solvePathCorners(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const glm::vec3& destination,
    float arrivalRadius,
    const AgentClearanceProfile& profile
) {
    const std::optional<ResolvedPathEndpoints> endpoints =
        resolvePathEndpoints(runtime, start, destination, profile);
    if (!endpoints.has_value()) {
        return std::nullopt;
    }

    if (canSolveDirectPath(runtime, *endpoints, profile)) {
        std::vector<glm::vec3> directCorners{};
        appendPathCorner(directCorners, endpoints->resolvedDestination, arrivalRadius);
        return SolvedPath{endpoints->resolvedDestination, std::move(directCorners)};
    }

    const auto solveWithVisibility = [&]() -> std::optional<SolvedPath> {
        const std::optional<std::vector<glm::vec3>> visibilityPath = solveVisibilityPath(
            runtime,
            endpoints->resolvedStart,
            endpoints->resolvedDestination,
            profile,
            endpoints->startCells
        );
        if (!visibilityPath.has_value()) {
            return std::nullopt;
        }
        std::vector<glm::vec3> visibilityCorners{};
        for (const glm::vec3& corner : *visibilityPath) {
            appendPathCorner(visibilityCorners, corner, kPolygonEpsilon);
        }
        if (visibilityCorners.empty() ||
            !intervalPathIsValid(runtime, endpoints->resolvedStart, visibilityCorners, profile)) {
            return std::nullopt;
        }
        return SolvedPath{endpoints->resolvedDestination, std::move(visibilityCorners)};
    };

    // Direction-dependent clearance creates a continuum of interval roots.
    // The finite visibility graph avoids that state explosion and tests direct routes explicitly.
    if (!profile.empty()) {
        if (std::optional<SolvedPath> visibilitySolution = solveWithVisibility();
            visibilitySolution.has_value()) {
            return visibilitySolution;
        }
    }

    if (std::optional<std::vector<glm::vec3>> intervalPath =
            solveIntervalPath(runtime, *endpoints, profile);
        intervalPath.has_value()) {
        std::vector<glm::vec3> corners = shortcutPathCorners(
            runtime,
            endpoints->resolvedStart,
            *intervalPath,
            kPolygonEpsilon,
            profile
        );
        if (intervalPathIsValid(runtime, endpoints->resolvedStart, corners, profile)) {
            return SolvedPath{endpoints->resolvedDestination, std::move(corners)};
        }
    }

    return profile.empty() ? solveWithVisibility() : std::nullopt;
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
    out << "version " << asset.version << "\n";
    out << "minimum_runtime_cell_area " << std::fixed << std::setprecision(6)
        << std::max(asset.minimumRuntimeCellArea, 0.0f) << "\n";
    for (const NavSourceTagOverride& overrideRecord : asset.sourceTagOverrides) {
        out << "source_tag_override " << std::quoted(overrideRecord.stableId) << " " << navSourceTagName(overrideRecord.tag) << "\n";
    }
    for (const NavPolygon& polygon : asset.polygons) {
        out << "polygon " << polygon.id << " " << std::fixed << std::setprecision(4) << polygon.elevationY;
        for (const glm::vec2& vertex : polygon.verticesXZ) {
            out << " " << vertex.x << " " << vertex.y;
        }
        out << "\n";
    }
    for (const NavLink& link : asset.links) {
        out << "link " << link.id << " " << link.fromPolygonId << " " << link.toPolygonId
            << " " << std::fixed << std::setprecision(4)
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
        const bool layerPolygonsAreDisjoint = sourcePolygonsAreDisjoint ||
            !bakeLayerHasInteriorPolygonOverlap(layer);
        std::vector<NavRuntimeCell> layerCells = layerPolygonsAreDisjoint
            ? bakeDisjointLayerRuntimeCells(layer, layerCellToPolygonIndices)
            : bakeLayerRuntimeCells(layer, layerCellToPolygonIndices);
        for (std::size_t localCellIndex = 0; localCellIndex < layerCells.size(); ++localCellIndex) {
            if (std::abs(polygonSignedArea(layerCells[localCellIndex].verticesXZ)) <
                std::max(runtime.asset.minimumRuntimeCellArea, 0.0f)) {
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

    runtime.graph.resize(runtime.bakedCells.size());
    for (std::size_t lhsIndex = 0; lhsIndex < runtime.bakedCells.size(); ++lhsIndex) {
        const NavRuntimeCell& lhs = runtime.bakedCells[lhsIndex];
        for (std::size_t rhsIndex = lhsIndex + 1u; rhsIndex < runtime.bakedCells.size(); ++rhsIndex) {
            const NavRuntimeCell& rhs = runtime.bakedCells[rhsIndex];
            if (std::abs(lhs.elevationY - rhs.elevationY) > kLayerGroupingEpsilon) {
                continue;
            }
            const auto portal = sharedPortal(lhs, rhs);
            if (!portal.has_value()) {
                continue;
            }
            runtime.graph[lhsIndex].push_back(NavGraphEdge{
                rhsIndex,
                false,
                -1,
                portal->a,
                portal->b,
                glm::vec3(0.0f),
                glm::vec3(0.0f)
            });
            runtime.graph[rhsIndex].push_back(NavGraphEdge{
                lhsIndex,
                false,
                -1,
                portal->a,
                portal->b,
                glm::vec3(0.0f),
                glm::vec3(0.0f)
            });
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
                solvePathCorners(solveView, startPosition, destination, arrivalRadius, clearanceProfile);
            if (path.has_value()) {
                result.destination = path->destination;
                result.pathCorners = path->corners;
                // Publish partial path so the agent can start moving toward the
                // destination before the final result is applied.
                {
                    std::lock_guard<std::mutex> lock(progress->mutex);
                    progress->partialPath = PartialPathResult{path->destination, path->corners};
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
                if (agent != nullptr) {
                    applyPathResult(*agent, partial->destination, std::move(partial->pathCorners));
                    pending.partialPathApplied = true;
                }
                ++it;
                continue;
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
        std::vector<glm::vec3> trimmedCorners = runtime.solveSnapshot != nullptr
            ? trimPathCornersFromCurrentPosition(
                  makeSolveView(*runtime.solveSnapshot),
                  transform->position,
                  *result->pathCorners,
                  agent->arrivalRadius,
                  clearanceProfile)
            : *result->pathCorners;
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

        const glm::vec2 planarDirection(direction.x, direction.z);
        if (glm::dot(planarDirection, planarDirection) > kPlaneEpsilon) {
            const float desiredYaw = glm::degrees(std::atan2(planarDirection.x, planarDirection.y));
            float updatedYaw = transform->rotationDeg.y;
            shortestYawStep(
                transform->rotationDeg.y,
                desiredYaw,
                agent.turnSpeedDeg * time.deltaSeconds,
                updatedYaw
            );
            transform->rotationDeg.y = updatedYaw;
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
