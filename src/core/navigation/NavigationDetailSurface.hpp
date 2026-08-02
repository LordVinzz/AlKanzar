#pragma once

#include <vector>

#include "core/navigation/NavigationDetailTypes.hpp"

namespace core::navigation_detail {

void ensureCCW(std::vector<glm::vec2>& vertices);
std::vector<glm::vec2> clipConvexPolygons(
    const std::vector<glm::vec2>& subject,
    const std::vector<glm::vec2>& clip
);
bool convexFootprintInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const std::vector<glm::vec2>& footprint,
    float elevationY,
    const std::vector<std::size_t>* candidateCells = nullptr
);

}  // namespace core::navigation_detail
