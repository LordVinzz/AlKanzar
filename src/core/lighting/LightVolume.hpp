#pragma once

#include <algorithm>
#include <vector>

#include <glm/vec3.hpp>

#include "core/ecs/Entity.hpp"

namespace core {

class LightVolume {
public:
    LightVolume() = default;
    LightVolume(const glm::vec3& minCorner, const glm::vec3& maxCorner)
        : minCorner_(minCorner), maxCorner_(maxCorner) {}

    void setBounds(const glm::vec3& minCorner, const glm::vec3& maxCorner) {
        minCorner_ = minCorner;
        maxCorner_ = maxCorner;
    }

    [[nodiscard]] const glm::vec3& minCorner() const { return minCorner_; }
    [[nodiscard]] const glm::vec3& maxCorner() const { return maxCorner_; }

    [[nodiscard]] bool intersectsSphere(const glm::vec3& center, float radius) const {
        float distanceSquared = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const float value = center[axis];
            if (value < minCorner_[axis]) {
                const float delta = minCorner_[axis] - value;
                distanceSquared += delta * delta;
            } else if (value > maxCorner_[axis]) {
                const float delta = value - maxCorner_[axis];
                distanceSquared += delta * delta;
            }
        }
        return distanceSquared <= radius * radius;
    }

    void attachStaticLight(EntityId lightEntity) {
        if (std::find(staticLightEntities_.begin(), staticLightEntities_.end(), lightEntity) == staticLightEntities_.end()) {
            staticLightEntities_.push_back(lightEntity);
        }
    }

    void detachStaticLight(EntityId lightEntity) {
        staticLightEntities_.erase(
            std::remove(staticLightEntities_.begin(), staticLightEntities_.end(), lightEntity),
            staticLightEntities_.end()
        );
    }

    void clearMovableLights() {
        movableLightEntities_.clear();
    }

    void addMovableLight(EntityId lightEntity) {
        if (std::find(movableLightEntities_.begin(), movableLightEntities_.end(), lightEntity) == movableLightEntities_.end()) {
            movableLightEntities_.push_back(lightEntity);
        }
    }

    void clearStaticLights() {
        staticLightEntities_.clear();
    }

    [[nodiscard]] const std::vector<EntityId>& staticLightEntities() const {
        return staticLightEntities_;
    }

    [[nodiscard]] const std::vector<EntityId>& movableLightEntities() const {
        return movableLightEntities_;
    }

private:
    glm::vec3 minCorner_{0.0f};
    glm::vec3 maxCorner_{0.0f};
    std::vector<EntityId> staticLightEntities_;
    std::vector<EntityId> movableLightEntities_;
};

}  // namespace core
