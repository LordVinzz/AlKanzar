#pragma once

namespace core::navigation_detail {

[[nodiscard]] float interpolateYawShortestPath(
    float currentYaw,
    float desiredYaw,
    float maxStep,
    float interpolationAlpha
);

}  // namespace core::navigation_detail
