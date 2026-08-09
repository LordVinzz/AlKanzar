#include "core/navigation/Navigation.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include <glm/vec2.hpp>

#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPath.hpp"
#include "core/navigation/NavigationDetailRuntime.hpp"

namespace core {
namespace {

constexpr float kDegreesToRadians = 0.017453292519943295f;
constexpr float kInitialRecoveryRetrySeconds = 0.25f;
constexpr float kMaximumRecoveryRetrySeconds = 2.0f;
constexpr std::uint8_t kMaximumRecoveryReplanAttempts = 4u;

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

glm::vec2 headingDirection(float yawDegrees) {
    const float yawRadians = yawDegrees * kDegreesToRadians;
    return glm::vec2(std::sin(yawRadians), std::cos(yawRadians));
}

float recoveryRetryDelay(std::uint8_t attempt) {
    float delay = kInitialRecoveryRetrySeconds;
    for (std::uint8_t index = 1u; index < attempt; ++index) {
        delay = std::min(delay * 2.0f, kMaximumRecoveryRetrySeconds);
    }
    return delay;
}

void beginRecoveryReplan(
    World& world,
    EntityId entity,
    NavAgentComponent& agent
) {
    agent.pathCorners.clear();
    agent.moving = false;
    agent.traversingLink = false;
    stopPhysicsMotor(world, entity, agent);
    if (agent.recoveryReplanActive) {
        return;
    }
    agent.recoveryReplanActive = true;
    agent.recoveryReplanRetrySeconds = 0.0f;
    agent.recoveryReplanAttempts = 0u;
}

}  // namespace

void NavigationSystem::reconcileAgentsAfterPhysics(
    World& world,
    const NavigationRuntime& runtime,
    const TimeContext& time,
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
        if (agent.recoveryReplanActive) {
            agent.recoveryReplanRetrySeconds = std::max(
                0.0f,
                agent.recoveryReplanRetrySeconds - time.deltaSeconds
            );
        }
        if (!agent.physicsStepStart.has_value()) {
            continue;
        }
        const glm::vec3 stepStart = *agent.physicsStepStart;
        const glm::vec3 stepStartRotation =
            agent.physicsStepStartRotationDeg;
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
            headingDirection(transform->rotationDeg.y);
        if (!navigation_detail::pointInsideAuthoredWalkableSurfaceWithClearance(
                solveView,
                transform->position,
                clearance,
                travelDirection)) {
            transform->position = stepStart;
            transform->rotationDeg = stepStartRotation;
            world.markTransformsDirty(entity);
            ++boundaryRecoveries_;
            if (agent.destination.has_value()) {
                beginRecoveryReplan(world, entity, agent);
            } else {
                stopPhysicsMotor(world, entity, agent);
            }
        }

        const auto attemptRecoveryReplan = [&]() {
            if (!agent.recoveryReplanActive) {
                return;
            }
            if (!agent.destination.has_value()) {
                agent.recoveryReplanActive = false;
                agent.recoveryReplanRetrySeconds = 0.0f;
                agent.recoveryReplanAttempts = 0u;
                return;
            }
            if (pendingPathRequests_.contains(entity) ||
                agent.recoveryReplanRetrySeconds > 0.0f) {
                return;
            }
            if (agent.recoveryReplanAttempts >=
                kMaximumRecoveryReplanAttempts) {
                agent.destination.reset();
                agent.recoveryReplanActive = false;
                agent.recoveryReplanRetrySeconds = 0.0f;
                agent.recoveryReplanAttempts = 0u;
                ++abandonedRecoveryReplans_;
                return;
            }

            ++agent.recoveryReplanAttempts;
            agent.recoveryReplanRetrySeconds = recoveryRetryDelay(
                agent.recoveryReplanAttempts
            );
            if (requestAgentDestinationInternal(
                    world,
                    runtime,
                    scheduler,
                    entity,
                    *agent.destination,
                    true)) {
                ++collisionReplans_;
            }
        };
        if (agent.recoveryReplanActive) {
            attemptRecoveryReplan();
            continue;
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
        beginRecoveryReplan(world, entity, agent);
        if (!destination.has_value()) {
            agent.recoveryReplanActive = false;
            continue;
        }
        agent.destination = destination;
        attemptRecoveryReplan();
    }
}

}  // namespace core
