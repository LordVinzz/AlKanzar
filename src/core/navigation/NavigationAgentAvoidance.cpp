#include "core/navigation/NavigationAgentAvoidance.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "core/ecs/World.hpp"
#include "core/transform/TransformMath.hpp"

namespace core::navigation_detail {
namespace {

constexpr float kAvoidancePadding = 0.08f;
constexpr float kAvoidanceHorizonSeconds = 0.75f;
constexpr float kVelocityEpsilon = 1.0e-5f;

struct AvoidanceBody {
    EntityId entity{};
    glm::vec2 center{0.0f};
    glm::vec2 velocity{0.0f};
    float radius{0.0f};
    float minY{0.0f};
    float maxY{0.0f};
    bool drivenAgent{false};
};

float maximumAxisScale(const glm::mat4& modelMatrix) {
    return std::max({
        glm::length(glm::vec3(modelMatrix[0])),
        glm::length(glm::vec3(modelMatrix[1])),
        glm::length(glm::vec3(modelMatrix[2]))
    });
}

glm::vec2 fallbackPairDirection(EntityId self, EntityId other) {
    if (self.index != other.index) {
        return self.index < other.index
            ? glm::vec2(1.0f, 0.0f)
            : glm::vec2(-1.0f, 0.0f);
    }
    return self.generation < other.generation
        ? glm::vec2(1.0f, 0.0f)
        : glm::vec2(-1.0f, 0.0f);
}

bool verticalRangesOverlap(const AvoidanceBody& a, const AvoidanceBody& b) {
    return a.minY < b.maxY - kVelocityEpsilon &&
        a.maxY > b.minY + kVelocityEpsilon;
}

std::vector<AvoidanceBody> collectBodies(World& world) {
    std::vector<AvoidanceBody> bodies{};
    bodies.reserve(world.rigidbodies.size());
    for (EntityId entity : world.rigidbodies.entities()) {
        const TransformComponent* transform = world.transforms.tryGet(entity);
        if (transform == nullptr) {
            continue;
        }
        const BoxColliderComponent* box = world.boxColliders.tryGet(entity);
        const SphereColliderComponent* sphere = world.sphereColliders.tryGet(entity);
        if (box == nullptr && sphere == nullptr) {
            continue;
        }

        const glm::mat4 modelMatrix = composeTransform(*transform);
        AvoidanceBody body{};
        body.entity = entity;
        if (box != nullptr) {
            const OrientedBox oriented = makeOrientedBox(modelMatrix, *box);
            body.center = glm::vec2(oriented.center.x, oriented.center.z);
            body.radius = glm::length(glm::vec2(
                oriented.halfExtents.x,
                oriented.halfExtents.z
            ));
            body.minY = oriented.aabb.min.y;
            body.maxY = oriented.aabb.max.y;
        } else {
            const glm::vec3 center = glm::vec3(
                modelMatrix * glm::vec4(sphere->center, 1.0f)
            );
            const float radius = sphere->radius *
                std::max(maximumAxisScale(modelMatrix), 0.001f);
            body.center = glm::vec2(center.x, center.z);
            body.radius = radius;
            body.minY = center.y - radius;
            body.maxY = center.y + radius;
        }

        const RigidbodyComponent& rigidbody = world.rigidbodies.get(entity);
        const NavAgentComponent* agent = world.navAgents.tryGet(entity);
        body.drivenAgent = agent != nullptr && !rigidbody.isKinematic &&
            !agent->traversingLink;
        const glm::vec3 velocity = body.drivenAgent
            ? agent->desiredVelocity
            : rigidbody.velocity;
        body.velocity = glm::vec2(velocity.x, velocity.z);
        bodies.push_back(body);
    }
    return bodies;
}

}  // namespace

std::size_t applyAgentLocalAvoidance(World& world) {
    const std::vector<AvoidanceBody> bodies = collectBodies(world);
    std::size_t adjustmentCount = 0u;
    for (const AvoidanceBody& self : bodies) {
        if (!self.drivenAgent) {
            continue;
        }
        NavAgentComponent* agent = world.navAgents.tryGet(self.entity);
        if (agent == nullptr) {
            continue;
        }
        const glm::vec2 preferred(
            agent->desiredVelocity.x,
            agent->desiredVelocity.z
        );
        const float preferredSpeed = glm::length(preferred);
        if (preferredSpeed <= kVelocityEpsilon) {
            continue;
        }

        glm::vec2 correction(0.0f);
        for (const AvoidanceBody& other : bodies) {
            if (other.entity == self.entity ||
                !verticalRangesOverlap(self, other)) {
                continue;
            }
            const glm::vec2 offset = other.center - self.center;
            const float distance = glm::length(offset);
            const glm::vec2 direction = distance > kVelocityEpsilon
                ? offset / distance
                : fallbackPairDirection(self.entity, other.entity);
            const float safeDistance = std::max(
                self.radius + other.radius + kAvoidancePadding,
                kAvoidancePadding
            );

            if (distance < safeDistance) {
                const float proximity = std::clamp(
                    (safeDistance - distance) / safeDistance,
                    0.0f,
                    1.0f
                );
                correction -= direction * preferredSpeed * proximity;
            }

            const glm::vec2 relativeVelocity = preferred - other.velocity;
            const float relativeSpeedSquared = glm::dot(
                relativeVelocity,
                relativeVelocity
            );
            if (relativeSpeedSquared <= kVelocityEpsilon * kVelocityEpsilon) {
                continue;
            }
            const float timeToClosest = glm::dot(
                offset,
                relativeVelocity
            ) / relativeSpeedSquared;
            if (timeToClosest <= 0.0f ||
                timeToClosest > kAvoidanceHorizonSeconds) {
                continue;
            }
            const glm::vec2 closestOffset =
                offset - relativeVelocity * timeToClosest;
            const float closestDistance = glm::length(closestOffset);
            if (closestDistance >= safeDistance) {
                continue;
            }

            const float collisionRisk =
                (1.0f - closestDistance / safeDistance) *
                (1.0f - timeToClosest / kAvoidanceHorizonSeconds);
            const glm::vec2 tangent(-direction.y, direction.x);
            correction += tangent * preferredSpeed * collisionRisk;
        }

        glm::vec2 adjusted = preferred + correction;
        const glm::vec2 forward = preferred / preferredSpeed;
        const float forwardSpeed = glm::dot(adjusted, forward);
        if (forwardSpeed < 0.0f) {
            adjusted -= forward * forwardSpeed;
        }
        const float adjustedSpeed = glm::length(adjusted);
        if (adjustedSpeed > agent->moveSpeed && adjustedSpeed > kVelocityEpsilon) {
            adjusted *= agent->moveSpeed / adjustedSpeed;
        }
        if (glm::distance(adjusted, preferred) <= kVelocityEpsilon) {
            continue;
        }
        agent->desiredVelocity.x = adjusted.x;
        agent->desiredVelocity.z = adjusted.y;
        ++adjustmentCount;
    }
    return adjustmentCount;
}

}  // namespace core::navigation_detail
