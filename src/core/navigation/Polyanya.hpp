#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

namespace core {

struct NavGraphEdge;
struct NavRuntimeCell;

namespace navigation_detail {

class PolyanyaMesh;

struct PolyanyaPath {
    // The resolved start is excluded and the resolved destination is included.
    std::vector<glm::vec3> corners{};
    double length{0.0};
    std::size_t expandedNodes{0u};
};

// Builds the conforming convex mesh required by Polyanya. Runtime cells may
// contain partial shared edges (T-junctions), so every portal endpoint is
// inserted before shared sub-edges are welded and checked for reciprocity.
std::shared_ptr<const PolyanyaMesh> buildPolyanyaMesh(
    const std::vector<NavRuntimeCell>& cells,
    const std::vector<std::vector<NavGraphEdge>>& graph,
    std::string* error = nullptr
);

std::optional<PolyanyaPath> findPolyanyaPath(
    const PolyanyaMesh& mesh,
    const glm::vec3& start,
    const glm::vec3& destination,
    const std::vector<std::size_t>& startCells,
    const std::vector<std::size_t>& targetCells,
    const std::atomic<bool>* cancelled = nullptr
);

} // namespace navigation_detail
} // namespace core
