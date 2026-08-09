#include "core/navigation/Navigation.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPath.hpp"

#include <atomic>
#include <optional>

namespace core {

using namespace navigation_detail;

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
        makeNavigationCounter("Local Avoidance Adjustments", static_cast<std::int64_t>(localAvoidanceAdjustments_)),
        makeNavigationCounter("Boundary Recoveries", static_cast<std::int64_t>(boundaryRecoveries_)),
        makeNavigationCounter("Collision Replans", static_cast<std::int64_t>(collisionReplans_)),
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

}  // namespace core
