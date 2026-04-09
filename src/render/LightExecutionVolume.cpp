#include "LightExecutionVolume.hpp"

#include <algorithm>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace render {

namespace {

void eraseLightIndex(std::vector<int>& indices, int lightIndex) {
    indices.erase(std::remove(indices.begin(), indices.end(), lightIndex), indices.end());
}

}  // namespace

LightExecutionVolume::LightExecutionVolume(const glm::vec3& minCorner, const glm::vec3& maxCorner) {
    setBounds(minCorner, maxCorner);
}

void LightExecutionVolume::setBounds(const glm::vec3& minCorner, const glm::vec3& maxCorner) {
    minCorner_ = glm::min(minCorner, maxCorner);
    maxCorner_ = glm::max(minCorner, maxCorner);
}

bool LightExecutionVolume::containsPoint(const glm::vec3& point) const {
    return point.x >= minCorner_.x && point.y >= minCorner_.y && point.z >= minCorner_.z &&
           point.x <= maxCorner_.x && point.y <= maxCorner_.y && point.z <= maxCorner_.z;
}

bool LightExecutionVolume::intersectsSphere(const glm::vec3& center, float radius) const {
    const glm::vec3 clamped = glm::clamp(center, minCorner_, maxCorner_);
    const glm::vec3 delta = center - clamped;
    return glm::dot(delta, delta) <= radius * radius;
}

void LightExecutionVolume::attachStaticLight(int lightIndex) {
    if (std::find(staticLightIndices_.begin(), staticLightIndices_.end(), lightIndex) == staticLightIndices_.end()) {
        staticLightIndices_.push_back(lightIndex);
    }
}

void LightExecutionVolume::detachStaticLight(int lightIndex) {
    eraseLightIndex(staticLightIndices_, lightIndex);
}

void LightExecutionVolume::clearMovableLights() {
    movableLightIndices_.clear();
}

void LightExecutionVolume::addMovableLight(int lightIndex) {
    if (std::find(movableLightIndices_.begin(), movableLightIndices_.end(), lightIndex) == movableLightIndices_.end()) {
        movableLightIndices_.push_back(lightIndex);
    }
}

}  // namespace render
