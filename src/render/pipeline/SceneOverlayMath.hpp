#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace render::detail {

[[nodiscard]] glm::mat4 makeOrientationFromDirection(const glm::vec3& direction);
[[nodiscard]] glm::vec3 normalizeOr(const glm::vec3& value, const glm::vec3& fallback);
[[nodiscard]] glm::mat4 makeUnscaledSelectionAxisModel(
    const glm::mat4& transformMatrix,
    float axisScale
);

}  // namespace render::detail
