#include "PolyanyaTopology.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace core::navigation_detail {

double cross(const polyanya::Point& lhs, const polyanya::Point& rhs) {
    return lhs.x * rhs.y - lhs.y * rhs.x;
}

double signedArea(const std::vector<polyanya::Point>& polygon) {
    double area = 0.0;
    for (std::size_t index = 0u; index < polygon.size(); ++index) {
        const polyanya::Point& current = polygon[index];
        const polyanya::Point& next = polygon[(index + 1u) % polygon.size()];
        area += cross(current, next);
    }
    return area * 0.5;
}

bool isConvexCounterClockwise(
    const std::vector<polyanya::Point>& polygon
) {
    if (polygon.size() < 3u || signedArea(polygon) <= kTopologyEpsilon) {
        return false;
    }
    for (std::size_t index = 0u; index < polygon.size(); ++index) {
        const polyanya::Point& previous = polygon[
            (index + polygon.size() - 1u) % polygon.size()];
        const polyanya::Point& current = polygon[index];
        const polyanya::Point& next = polygon[
            (index + 1u) % polygon.size()];
        const polyanya::Point incoming = current - previous;
        const polyanya::Point outgoing = next - current;
        const double scale = std::max(
            std::sqrt(incoming.distance_sq({0.0, 0.0})),
            std::sqrt(outgoing.distance_sq({0.0, 0.0}))
        );
        // Cross products scale with edge length. Long world-boundary edges can
        // turn a sub-millimetric serialized/snap deviation into a sizeable raw
        // cross value even though the vertex is effectively collinear.
        if (cross(incoming, outgoing) <
            -kJunctionSnapEpsilon * scale) {
            return false;
        }
    }
    return true;
}

bool pointOnSegment(
    const polyanya::Point& point,
    const polyanya::Point& a,
    const polyanya::Point& b,
    double tolerance
) {
    const polyanya::Point edge = b - a;
    const double length = std::sqrt(edge.distance_sq({0.0, 0.0}));
    if (length <= tolerance) {
        return point.distance(a) <= tolerance;
    }
    if (std::abs(cross(edge, point - a)) > tolerance * length) {
        return false;
    }
    const double projection =
        (point.x - a.x) * edge.x + (point.y - a.y) * edge.y;
    return projection >= -tolerance * length &&
        projection <= edge.distance_sq({0.0, 0.0}) +
            tolerance * length;
}

double segmentParameter(
    const polyanya::Point& point,
    const polyanya::Point& a,
    const polyanya::Point& b
) {
    const polyanya::Point edge = b - a;
    const double denominator = edge.distance_sq({0.0, 0.0});
    if (denominator <= kTopologyEpsilon * kTopologyEpsilon) {
        return 0.0;
    }
    return ((point.x - a.x) * edge.x + (point.y - a.y) * edge.y) /
        denominator;
}

polyanya::Point interpolate(
    const polyanya::Point& a,
    const polyanya::Point& b,
    double t
) {
    return a + (b - a) * t;
}

std::size_t WeldKeyHash::operator()(const WeldKey& key) const noexcept {
    std::size_t result = std::hash<std::size_t>{}(key.component);
    result ^= std::hash<std::int64_t>{}(key.x) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
    result ^= std::hash<std::int64_t>{}(key.y) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
    return result;
}

std::size_t UndirectedEdgeKeyHash::operator()(const UndirectedEdgeKey& key) const noexcept {
    return std::hash<int>{}(key.low) ^ (std::hash<int>{}(key.high) << 1u);
}

bool graphContainsPortal(
    const std::vector<std::vector<NavGraphEdge>>& graph,
    std::size_t fromCell,
    std::size_t toCell,
    const polyanya::Point& midpoint,
    std::vector<std::vector<std::uint8_t>>* matchedPortals
) {
    if (fromCell >= graph.size()) {
        return false;
    }
    bool found = false;
    for (std::size_t edgeIndex = 0u;
         edgeIndex < graph[fromCell].size();
         ++edgeIndex) {
        const NavGraphEdge& edge = graph[fromCell][edgeIndex];
        if (edge.viaLink || edge.targetCellIndex != toCell) {
            continue;
        }
        if (pointOnSegment(
                midpoint,
                {edge.portalA.x, edge.portalA.y},
                {edge.portalB.x, edge.portalB.y},
                kJunctionSnapEpsilon)) {
            if (matchedPortals != nullptr &&
                fromCell < matchedPortals->size() &&
                edgeIndex < (*matchedPortals)[fromCell].size()) {
                (*matchedPortals)[fromCell][edgeIndex] = 1u;
            }
            found = true;
        }
    }
    return found;
}

std::vector<std::size_t> planarComponents(
    std::size_t cellCount,
    const std::vector<std::vector<NavGraphEdge>>& graph
) {
    const std::size_t invalid = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> components(cellCount, invalid);
    std::vector<std::size_t> pending{};
    std::size_t nextComponent = 0u;
    for (std::size_t seed = 0u; seed < cellCount; ++seed) {
        if (components[seed] != invalid) {
            continue;
        }
        components[seed] = nextComponent;
        pending.push_back(seed);
        while (!pending.empty()) {
            const std::size_t cell = pending.back();
            pending.pop_back();
            if (cell >= graph.size()) {
                continue;
            }
            for (const NavGraphEdge& edge : graph[cell]) {
                if (edge.viaLink || edge.targetCellIndex >= cellCount ||
                    components[edge.targetCellIndex] != invalid) {
                    continue;
                }
                components[edge.targetCellIndex] = nextComponent;
                pending.push_back(edge.targetCellIndex);
            }
        }
        ++nextComponent;
    }
    return components;

}

}  // namespace core::navigation_detail
