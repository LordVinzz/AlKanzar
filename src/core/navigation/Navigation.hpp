#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "core/app/FrameData.hpp"
#include "core/app/TimeContext.hpp"
#include "core/ecs/World.hpp"
#include "core/scene/SceneBlueprint.hpp"
#include "core/systems/TaskScheduler.hpp"
#include "render/engine/RenderTypes.hpp"
#include "render/resources/Profiling.hpp"

namespace core {

class ProfilerService;

namespace navigation_detail {
class PolyanyaMesh;
}

struct NavSourceTagOverride {
    std::string stableId{};
    NavSourceTag tag{NavSourceTag::Ignored};
};

struct NavPolygon {
    int id{-1};
    float elevationY{0.0f};
    std::vector<glm::vec2> verticesXZ{};
};

struct NavLink {
    int id{-1};
    int fromPolygonId{-1};
    int toPolygonId{-1};
    glm::vec3 fromPoint{0.0f};
    glm::vec3 toPoint{0.0f};
    bool bidirectional{true};
};

struct NavMeshAsset {
    int version{1};
    // Generation constraint: triangles below this world-space area are not
    // emitted into the navmesh asset. Zero disables the constraint.
    float minimumRuntimeCellArea{0.0f};
    // Zero keeps the generated mesh unconstrained. A positive value limits
    // every generated polygon edge to this world-space length.
    float maximumPolygonEdgeLength{0.0f};
    std::vector<NavSourceTagOverride> sourceTagOverrides{};
    std::vector<NavPolygon> polygons{};
    std::vector<NavLink> links{};
};

struct NavHitResult {
    int polygonId{-1};
    glm::vec3 position{0.0f};
    float distance{0.0f};
};

struct NavRuntimeCell {
    float elevationY{0.0f};
    std::vector<glm::vec2> verticesXZ{};
};

struct NavGraphEdge {
    std::size_t targetCellIndex{0u};
    bool viaLink{false};
    int linkId{-1};
    // Directed planar portals are stored left-to-right as seen while moving
    // from the owning cell to targetCellIndex.
    glm::vec2 portalA{0.0f};
    glm::vec2 portalB{0.0f};
    glm::vec3 linkStartPoint{0.0f};
    glm::vec3 linkEndPoint{0.0f};
};

struct NavigationSolveSnapshot {
    NavMeshAsset asset{};
    std::unordered_map<int, std::size_t> polygonIndexById{};
    std::vector<glm::vec2> polygonCenters{};
    std::vector<NavRuntimeCell> bakedCells{};
    std::vector<glm::vec2> bakedCellCenters{};
    std::vector<glm::vec2> bakedCellMinXZ{};
    std::vector<glm::vec2> bakedCellMaxXZ{};
    std::vector<std::vector<std::uint8_t>> bakedCellBoundaryVertices{};
    std::vector<std::vector<std::size_t>> polygonToCellIndices{};
    std::vector<std::vector<std::size_t>> cellToPolygonIndices{};
    std::vector<std::vector<NavGraphEdge>> graph{};
    std::shared_ptr<const navigation_detail::PolyanyaMesh> polyanyaMesh{};
    bool bakedCellsHaveInteriorOverlap{false};
};

struct NavigationEditorState {
    bool testMoveMode{false};
    bool polygonCaptureActive{false};
    float polygonCaptureElevation{0.0f};
    std::vector<glm::vec2> polygonCaptureVertices{};
    int pendingLinkFromPolygonId{-1};
    int pendingLinkToPolygonId{-1};
    glm::vec3 pendingLinkFromPoint{0.0f};
    glm::vec3 pendingLinkToPoint{0.0f};
    bool pendingLinkBidirectional{true};
};

struct NavigationRuntime {
    std::string assetPath{};
    NavMeshAsset asset{};
    std::unordered_map<int, std::size_t> polygonIndexById{};
    std::vector<glm::vec2> polygonCenters{};
    std::vector<NavRuntimeCell> bakedCells{};
    std::vector<glm::vec2> bakedCellCenters{};
    std::vector<glm::vec2> bakedCellMinXZ{};
    std::vector<glm::vec2> bakedCellMaxXZ{};
    std::vector<std::vector<std::uint8_t>> bakedCellBoundaryVertices{};
    std::vector<std::vector<std::size_t>> polygonToCellIndices{};
    std::vector<std::vector<std::size_t>> cellToPolygonIndices{};
    std::vector<std::vector<NavGraphEdge>> graph{};
    std::shared_ptr<const navigation_detail::PolyanyaMesh> polyanyaMesh{};
    bool bakedCellsHaveInteriorOverlap{false};
    std::shared_ptr<const NavigationSolveSnapshot> solveSnapshot{};
    std::uint64_t solveRevision{0u};
    std::string statusMessage{};
    std::string exactPathfindingWarning{};
    bool statusIsError{false};
    NavigationEditorState editor{};
};

const char* navSourceTagName(NavSourceTag tag);
bool tryParseNavSourceTag(const std::string& token, NavSourceTag& outTag);
std::string serializeNavMeshAsset(const NavMeshAsset& asset);
bool parseNavMeshAsset(const std::string& text, NavMeshAsset& outAsset, std::string* error = nullptr);

class NavigationSystem {
public:
    void setProfiler(ProfilerService* profiler) { profiler_ = profiler; }

    bool initializeScene(const SceneBlueprint& blueprint, World& world, NavigationRuntime& runtime) const;
    bool loadAsset(NavigationRuntime& runtime) const;
    bool reloadAsset(NavigationRuntime& runtime) const;
    bool saveAsset(const NavigationRuntime& runtime, std::string* error = nullptr) const;
    bool rebuildRuntime(NavigationRuntime& runtime, std::string* error = nullptr) const;
    bool generateFromTags(const World& world, NavigationRuntime& runtime, std::string* error = nullptr) const;

    std::optional<NavHitResult> hitTest(
        const NavigationRuntime& runtime,
        const render::CameraMatrices& camera,
        int viewportWidth,
        int viewportHeight,
        int mouseX,
        int mouseY
    ) const;

    bool setAgentDestination(
        World& world,
        const NavigationRuntime& runtime,
        EntityId agentEntity,
        const glm::vec3& destination
    ) const;
    bool requestAgentDestination(
        World& world,
        const NavigationRuntime& runtime,
        TaskScheduler& scheduler,
        EntityId agentEntity,
        const glm::vec3& destination
    ) const;
    void applyCompletedPathRequests(World& world, const NavigationRuntime& runtime) const;
    [[nodiscard]] std::vector<render::FrameCounterRecord> profilingCounters() const;

    void updateAgents(World& world, const NavigationRuntime& runtime, const TimeContext& time) const;
    void syncFrame(const World& world, const NavigationRuntime& runtime, FrameSceneData& frame) const;

    std::vector<EntityId> collectRenderableSelectionTargets(const World& world, EntityId selected) const;
    void applyTagOverride(World& world, NavigationRuntime& runtime, EntityId entity, NavSourceTag tag) const;
    bool capturePolygonClick(
        NavigationRuntime& runtime,
        const render::CameraMatrices& camera,
        int viewportWidth,
        int viewportHeight,
        int mouseX,
        int mouseY
    ) const;
    bool commitCapturedPolygon(NavigationRuntime& runtime, std::string* error = nullptr) const;
    void clearCapturedPolygon(NavigationRuntime& runtime) const;
    bool seedPendingLink(NavigationRuntime& runtime, int fromPolygonId, int toPolygonId, std::string* error = nullptr) const;
    bool commitPendingLink(NavigationRuntime& runtime, std::string* error = nullptr) const;

private:
    bool rebuildRuntimeInternal(
        NavigationRuntime& runtime,
        std::string* error,
        bool sourcePolygonsAreDisjoint
    ) const;

    struct PathSolveResult {
        EntityId agentEntity{};
        std::uint64_t requestId{0u};
        std::uint64_t solveRevision{0u};
        glm::vec3 startPosition{0.0f};
        glm::vec3 resolvedStart{0.0f};
        glm::vec3 destination{0.0f};
        std::optional<std::vector<glm::vec3>> pathCorners{};
        std::uint64_t durationUs{0u};
    };

public:
    struct PartialPathResult {
        glm::vec3 resolvedStart{0.0f};
        glm::vec3 destination{0.0f};
        std::vector<glm::vec3> pathCorners{};
    };

    struct PendingPathProgress {
        std::mutex mutex{};
        std::optional<PartialPathResult> partialPath{};
    };

private:
    struct PendingPathRequest {
        std::uint64_t requestId{0u};
        std::uint64_t solveRevision{0u};
        glm::vec3 startPosition{0.0f};
        glm::vec3 destination{0.0f};
        std::shared_ptr<PendingPathProgress> progress{};
        std::shared_ptr<std::atomic<bool>> cancelled{};
        bool partialPathApplied{false};
        AsyncTaskHandle<PathSolveResult> handle{};
    };

    void discardPendingPathRequest(EntityId entity) const;
    void invalidatePendingPathRequests() const;

    ProfilerService* profiler_{nullptr};
    mutable std::unordered_map<EntityId, PendingPathRequest> pendingPathRequests_{};
    mutable std::uint64_t nextPathRequestId_{1u};
    mutable std::uint64_t lastAsyncPathfindUs_{0u};
    mutable std::uint64_t failedPathRequests_{0u};
    mutable std::uint64_t stalePathResults_{0u};
};

}  // namespace core
