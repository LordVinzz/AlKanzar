#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/navigation/NavigationDetailTypes.hpp"

namespace core::navigation_detail {

NavigationSolveView makeSolveView(const NavigationRuntime& runtime);
NavigationSolveView makeSolveView(const NavigationSolveSnapshot& snapshot);
std::shared_ptr<const NavigationSolveSnapshot> buildSolveSnapshot(
    const NavigationRuntime& runtime
);
void appendPathCorner(
    std::vector<glm::vec3>& corners,
    const glm::vec3& point,
    float arrivalRadius
);
float pathLength(const glm::vec3& start, const std::vector<glm::vec3>& corners);
ParentPathData buildStableIdPaths(const World& world);
NavSourceTag defaultTagForLayer(render::RenderLayer layer);

}  // namespace core::navigation_detail
