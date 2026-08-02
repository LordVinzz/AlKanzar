#include "core/navigation/Navigation.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPath.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

#include "core/profiling/ProfilerService.hpp"

namespace core {

using namespace navigation_detail;

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

}  // namespace core
