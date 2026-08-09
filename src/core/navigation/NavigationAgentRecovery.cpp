#include "core/navigation/Navigation.hpp"

#include <optional>
#include <utility>

#include <glm/vec2.hpp>

#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPath.hpp"
#include "core/navigation/NavigationDetailRuntime.hpp"

namespace core {
namespace {

void stopPhysicsMotor(
    World& world,
    EntityId entity,
    NavAgentComponent& agent
) {
    agent.desiredVelocity = glm::vec3(0.0f);
    if (RigidbodyComponent* rigidbody = world.rigidbodies.tryGet(entity)) {
        rigidbody->velocity = glm::vec3(0.0f);
    }
}

}  // namespace

void NavigationSystem::reconcileAgentsAfterPhysics(
    World& world,
    const NavigationRuntime& runtime,
    TaskScheduler& scheduler
) const {
    const std::shared_ptr<const NavigationSolveSnapshot> snapshot =
        runtime.solveSnapshot;
    if (snapshot == nullptr || snapshot->bakedCells.empty()) {
        for (NavAgentComponent& agent : world.navAgents.values()) {
            agent.physicsStepStart.reset();
        }
        return;
    }
    const navigation_detail::NavigationSolveView solveView =
        navigation_detail::makeSolveView(*snapshot);

    for (EntityId entity : world.navAgents.entities()) {
        NavAgentComponent& agent = world.navAgents.get(entity);
        if (!agent.physicsStepStart.has_value()) {
            continue;
        }
        const glm::vec3 stepStart = *agent.physicsStepStart;
        agent.physicsStepStart.reset();

        TransformComponent* transform = world.transforms.tryGet(entity);
        RigidbodyComponent* rigidbody = world.rigidbodies.tryGet(entity);
        if (transform == nullptr || rigidbody == nullptr ||
            rigidbody->isKinematic) {
            continue;
        }
        if (agent.traversingLink) {
            continue;
        }

        const navigation_detail::AgentClearanceProfile clearance =
            navigation_detail::resolveAgentClearanceProfile(
                world,
                entity,
                agent
            );
        const glm::vec2 travelDirection =
            navigation_detail::travelDirectionForSegment(
                stepStart,
                stepStart + agent.desiredVelocity
            );
        if (!navigation_detail::pointInsideAuthoredWalkableSurfaceWithClearance(
                solveView,
                transform->position,
                clearance,
                travelDirection)) {
            transform->position = stepStart;
            stopPhysicsMotor(world, entity, agent);
            world.markTransformsDirty(entity);
            ++boundaryRecoveries_;
        }

        if (!agent.moving || agent.pathCorners.empty()) {
            continue;
        }
        std::vector<glm::vec3> trimmedCorners =
            navigation_detail::trimPathCornersFromCurrentPosition(
                solveView,
                transform->position,
                agent.pathCorners,
                agent.arrivalRadius,
                clearance
            );
        if (trimmedCorners.empty()) {
            agent.pathCorners.clear();
            agent.destination.reset();
            agent.moving = false;
            agent.traversingLink = false;
            stopPhysicsMotor(world, entity, agent);
            continue;
        }
        if (navigation_detail::pathSegmentsAreValid(
                solveView,
                transform->position,
                trimmedCorners,
                clearance)) {
            agent.pathCorners = std::move(trimmedCorners);
            continue;
        }

        const std::optional<glm::vec3> destination = agent.destination;
        agent.pathCorners.clear();
        agent.moving = false;
        agent.traversingLink = false;
        stopPhysicsMotor(world, entity, agent);
        if (!destination.has_value() ||
            pendingPathRequests_.contains(entity)) {
            continue;
        }
        if (requestAgentDestination(
                world,
                runtime,
                scheduler,
                entity,
                *destination)) {
            ++collisionReplans_;
        } else {
            agent.destination.reset();
        }
    }
}

}  // namespace core
