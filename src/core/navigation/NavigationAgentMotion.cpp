#include "core/navigation/Navigation.hpp"
#include "core/navigation/NavigationDetailAgents.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

namespace core {

using namespace navigation_detail;

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


}  // namespace core
