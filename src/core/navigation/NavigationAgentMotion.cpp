#include "core/navigation/Navigation.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

#include "core/navigation/NavigationAgentAvoidance.hpp"
#include "core/navigation/NavigationDetailAgents.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPath.hpp"
#include "core/navigation/NavigationDetailRuntime.hpp"

namespace core {
namespace {

constexpr float kDegreesToRadians = 0.017453292519943295f;
constexpr float kHeadingInterpolationRate = 12.0f;

bool usesPhysicsMotor(const World& world, EntityId entity) {
    const RigidbodyComponent* rigidbody = world.rigidbodies.tryGet(entity);
    return rigidbody != nullptr && !rigidbody->isKinematic &&
        (world.boxColliders.contains(entity) ||
         world.sphereColliders.contains(entity));
}

void requestIdleAnimation(
    AnimatedModelComponent* animation,
    const LocomotionComponent* locomotion
) {
    if (animation == nullptr || locomotion == nullptr ||
        locomotion->idleClip < 0) {
        return;
    }
    const int currentOrNext = animation->nextClip >= 0
        ? animation->nextClip
        : animation->currentClip;
    if (currentOrNext != locomotion->idleClip) {
        animation->requestedClip = locomotion->idleClip;
    }
}

void requestWalkAnimation(
    AnimatedModelComponent* animation,
    const LocomotionComponent* locomotion
) {
    if (animation == nullptr || locomotion == nullptr ||
        locomotion->walkClip < 0) {
        return;
    }
    const int currentOrNext = animation->nextClip >= 0
        ? animation->nextClip
        : animation->currentClip;
    if (currentOrNext != locomotion->walkClip) {
        animation->requestedClip = locomotion->walkClip;
    }
    animation->speed = 1.0f;
}

float headingAlignmentScale(
    const glm::vec3& velocity,
    const TransformComponent& transform
) {
    const glm::vec2 planarVelocity(velocity.x, velocity.z);
    const float planarSpeed = glm::length(planarVelocity);
    if (planarSpeed <= navigation_detail::kPlaneEpsilon) {
        return 1.0f;
    }
    const float yawRadians = transform.rotationDeg.y * kDegreesToRadians;
    const glm::vec2 heading(
        std::sin(yawRadians),
        std::cos(yawRadians)
    );
    const float alignment = std::clamp(
        glm::dot(heading, planarVelocity / planarSpeed),
        0.0f,
        1.0f
    );
    return alignment * alignment;
}

bool updateAgentHeading(
    const NavAgentComponent& agent,
    const glm::vec3& velocity,
    float deltaSeconds,
    TransformComponent& transform
) {
    const glm::vec2 planarDirection(velocity.x, velocity.z);
    if (glm::dot(planarDirection, planarDirection) <=
        navigation_detail::kPlaneEpsilon *
            navigation_detail::kPlaneEpsilon) {
        return false;
    }
    const float desiredYaw = glm::degrees(
        std::atan2(planarDirection.x, planarDirection.y)
    );
    const float previousYaw = transform.rotationDeg.y;
    const float safeDeltaSeconds = std::max(deltaSeconds, 0.0f);
    const float interpolationAlpha = 1.0f - std::exp(
        -kHeadingInterpolationRate * safeDeltaSeconds
    );
    transform.rotationDeg.y =
        navigation_detail::interpolateYawShortestPath(
            transform.rotationDeg.y,
            desiredYaw,
            agent.turnSpeedDeg * safeDeltaSeconds,
            interpolationAlpha
        );
    return std::abs(transform.rotationDeg.y - previousYaw) > 1.0e-5f;
}

bool canAdvanceReachedCorner(
    const World& world,
    EntityId entity,
    const NavAgentComponent& agent,
    const TransformComponent& transform,
    const navigation_detail::NavigationSolveView& solveView,
    bool physicsDriven
) {
    if (!physicsDriven) {
        return !agent.pathCorners.empty() &&
            glm::distance(
                transform.position,
                agent.pathCorners.front()
            ) <= agent.arrivalRadius;
    }
    return navigation_detail::canAdvancePathCorner(
        solveView,
        transform.position,
        agent.pathCorners,
        agent.arrivalRadius,
        navigation_detail::resolveAgentClearanceProfile(
            world,
            entity,
            agent
        )
    );
}

}  // namespace

void NavigationSystem::updateAgents(
    World& world,
    const NavigationRuntime& runtime,
    const TimeContext& time
) const {
    const navigation_detail::NavigationSolveView solveView =
        navigation_detail::makeSolveView(runtime);
    for (EntityId entity : world.navAgents.entities()) {
        NavAgentComponent& agent = world.navAgents.get(entity);
        agent.desiredVelocity = glm::vec3(0.0f);
        agent.physicsStepStart.reset();

        TransformComponent* transform = world.transforms.tryGet(entity);
        if (transform == nullptr) {
            agent.traversingLink = false;
            continue;
        }
        const bool physicsDriven = usesPhysicsMotor(world, entity);
        if (physicsDriven) {
            agent.physicsStepStart = transform->position;
            agent.physicsStepStartRotationDeg = transform->rotationDeg;
        }

        AnimatedModelComponent* animation = world.animatedModels.tryGet(entity);
        const LocomotionComponent* locomotion = world.locomotion.tryGet(entity);
        if (!agent.moving || agent.pathCorners.empty()) {
            agent.moving = false;
            agent.traversingLink = false;
            requestIdleAnimation(animation, locomotion);
            continue;
        }

        while (canAdvanceReachedCorner(
                world,
                entity,
                agent,
                *transform,
                solveView,
                physicsDriven)) {
            if (!physicsDriven) {
                transform->position = agent.pathCorners.front();
                world.markTransformsDirty(entity);
            }
            agent.pathCorners.erase(agent.pathCorners.begin());
            agent.traversingLink = false;
        }
        if (agent.pathCorners.empty()) {
            agent.destination.reset();
            agent.moving = false;
            requestIdleAnimation(animation, locomotion);
            continue;
        }

        const glm::vec3 target = agent.pathCorners.front();
        const glm::vec3 toTarget = target - transform->position;
        const float distance = glm::length(toTarget);
        const glm::vec3 direction = toTarget /
            std::max(distance, navigation_detail::kPlaneEpsilon);
        if (!agent.traversingLink) {
            agent.traversingLink = navigation_detail::segmentMatchesRuntimeLink(
                solveView,
                transform->position,
                target,
                std::max(
                    agent.arrivalRadius,
                    navigation_detail::kPolygonEpsilon * 8.0f
                )
            );
        }

        if (physicsDriven) {
            const float speed = time.deltaSeconds >
                    navigation_detail::kPlaneEpsilon
                ? std::min(agent.moveSpeed, distance / time.deltaSeconds)
                : 0.0f;
            agent.desiredVelocity = direction * speed;
        } else {
            if (updateAgentHeading(
                    agent,
                    direction,
                    time.deltaSeconds,
                    *transform)) {
                world.markTransformsDirty(entity);
            }
            const float step = agent.moveSpeed * time.deltaSeconds *
                headingAlignmentScale(direction, *transform);
            if (step >= distance) {
                transform->position = target;
                agent.pathCorners.erase(agent.pathCorners.begin());
                agent.traversingLink = false;
                if (agent.pathCorners.empty()) {
                    agent.destination.reset();
                    agent.moving = false;
                }
            } else {
                transform->position += direction * step;
            }
            world.markTransformsDirty(entity);
        }
        requestWalkAnimation(animation, locomotion);
    }

    localAvoidanceAdjustments_ +=
        navigation_detail::applyAgentLocalAvoidance(world);
    for (EntityId entity : world.navAgents.entities()) {
        NavAgentComponent& agent = world.navAgents.get(entity);
        if (!agent.physicsStepStart.has_value()) {
            continue;
        }
        TransformComponent* transform = world.transforms.tryGet(entity);
        if (transform != nullptr) {
            if (updateAgentHeading(
                    agent,
                    agent.desiredVelocity,
                    time.deltaSeconds,
                    *transform)) {
                world.markTransformsDirty(entity);
            }
            agent.desiredVelocity *= headingAlignmentScale(
                agent.desiredVelocity,
                *transform
            );
        }
    }
}

}  // namespace core
