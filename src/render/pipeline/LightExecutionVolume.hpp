#pragma once

#include <vector>

#include <glm/vec3.hpp>

namespace render {

class LightExecutionVolume {
public:
    LightExecutionVolume() = default;
    LightExecutionVolume(const glm::vec3& minCorner, const glm::vec3& maxCorner);

    void setBounds(const glm::vec3& minCorner, const glm::vec3& maxCorner);
    const glm::vec3& minCorner() const { return minCorner_; }
    const glm::vec3& maxCorner() const { return maxCorner_; }

    bool containsPoint(const glm::vec3& point) const;
    bool intersectsSphere(const glm::vec3& center, float radius) const;

    void attachStaticLight(int lightIndex);
    void detachStaticLight(int lightIndex);
    void clearMovableLights();
    void addMovableLight(int lightIndex);
    const std::vector<int>& staticLightIndices() const { return staticLightIndices_; }
    const std::vector<int>& movableLightIndices() const { return movableLightIndices_; }

private:
    glm::vec3 minCorner_{0.0f};
    glm::vec3 maxCorner_{0.0f};
    std::vector<int> staticLightIndices_;
    std::vector<int> movableLightIndices_;
};

}  // namespace render
