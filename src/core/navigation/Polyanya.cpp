#include "Polyanya.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <glm/geometric.hpp>

#include "Navigation.hpp"
#include "polyanya/expansion.h"
#include "polyanya/mesh.h"

// The interval expansion and heuristic are adapted from the reference
// implementation by Michael Cui and Daniel Harabor. See polyanya/LICENSE.txt.

namespace core::navigation_detail {
namespace {

constexpr double kTopologyEpsilon = 1.0e-4;
constexpr double kWeldEpsilon = 2.5e-5;
constexpr double kWeldQuantum = kWeldEpsilon;
constexpr double kJunctionSnapEpsilon = 8.0e-4;
constexpr double kSearchEpsilon = 1.0e-8;

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
    double tolerance = kTopologyEpsilon
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

struct WeldKey {
    std::size_t component{0u};
    std::int64_t x{0};
    std::int64_t y{0};

    friend bool operator==(const WeldKey&, const WeldKey&) = default;
};

struct WeldKeyHash {
    std::size_t operator()(const WeldKey& key) const noexcept {
        std::size_t result = std::hash<std::size_t>{}(key.component);
        result ^= std::hash<std::int64_t>{}(key.x) + 0x9e3779b9u +
            (result << 6u) + (result >> 2u);
        result ^= std::hash<std::int64_t>{}(key.y) + 0x9e3779b9u +
            (result << 6u) + (result >> 2u);
        return result;
    }
};

struct UndirectedEdgeKey {
    int low{-1};
    int high{-1};

    friend bool operator==(const UndirectedEdgeKey&, const UndirectedEdgeKey&) =
        default;
};

struct UndirectedEdgeKeyHash {
    std::size_t operator()(const UndirectedEdgeKey& key) const noexcept {
        return std::hash<int>{}(key.low) ^
            (std::hash<int>{}(key.high) << 1u);
    }
};

struct EdgeOccurrence {
    std::size_t cell{0u};
    std::size_t neighbourSlot{0u};
};

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

polyanya::Point rootPoint(
    const polyanya::Mesh& mesh,
    int root,
    const polyanya::Point& start
) {
    return root < 0 ? start : mesh.mesh_vertices[static_cast<std::size_t>(root)].p;
}

struct SearchNode {
    polyanya::SearchNode interval{};
    SearchNode* parent{nullptr};
};

struct SearchNodeCompare {
    bool operator()(const SearchNode* lhs, const SearchNode* rhs) const {
        if (lhs->interval.f == rhs->interval.f) {
            return lhs->interval.g < rhs->interval.g;
        }
        return lhs->interval.f > rhs->interval.f;
    }
};

bool samePoint(const polyanya::Point& lhs, const polyanya::Point& rhs) {
    return lhs == rhs;
}

} // namespace

class PolyanyaMesh {
public:
    polyanya::Mesh mesh{};
    std::vector<float> elevations{};
    std::vector<std::size_t> components{};
};

std::shared_ptr<const PolyanyaMesh> buildPolyanyaMesh(
    const std::vector<NavRuntimeCell>& cells,
    const std::vector<std::vector<NavGraphEdge>>& graph,
    std::string* error
) {
    const auto fail = [&](const std::string& message)
        -> std::shared_ptr<const PolyanyaMesh> {
        if (error != nullptr) {
            *error = message;
        }
        return {};
    };
    if (cells.empty()) {
        return fail("Polyanya mesh has no runtime cells");
    }
    if (graph.size() != cells.size()) {
        return fail("Polyanya graph/cell count mismatch");
    }

    const std::vector<std::size_t> components =
        planarComponents(cells.size(), graph);
    const std::size_t componentCount = components.empty()
        ? 0u
        : *std::max_element(components.begin(), components.end()) + 1u;
    std::vector<std::vector<polyanya::Point>> portalPointsByComponent(
        componentCount
    );
    std::vector<std::vector<polyanya::Point>> portalPointsByCell(
        cells.size()
    );
    for (std::size_t cellIndex = 0u; cellIndex < graph.size(); ++cellIndex) {
        if (components[cellIndex] >= portalPointsByComponent.size()) {
            continue;
        }
        std::vector<polyanya::Point>& portalPoints =
            portalPointsByComponent[components[cellIndex]];
        for (const NavGraphEdge& edge : graph[cellIndex]) {
            if (edge.viaLink) {
                continue;
            }
            for (const polyanya::Point point : {
                     polyanya::Point{edge.portalA.x, edge.portalA.y},
                     polyanya::Point{edge.portalB.x, edge.portalB.y},
                 }) {
                auto canonical = std::find_if(
                    portalPoints.begin(),
                    portalPoints.end(),
                    [&](const polyanya::Point& existing) {
                        return existing.distance(point) <=
                            kJunctionSnapEpsilon;
                    }
                );
                if (canonical == portalPoints.end()) {
                    portalPoints.push_back(point);
                    canonical = portalPoints.end() - 1;
                }
                std::vector<polyanya::Point>& localPoints =
                    portalPointsByCell[cellIndex];
                if (std::none_of(
                        localPoints.begin(),
                        localPoints.end(),
                        [&](const polyanya::Point& existing) {
                            return existing == *canonical;
                        })) {
                    localPoints.push_back(*canonical);
                }
            }
        }
    }

    struct EdgeSplitPoint {
        double parameter{0.0};
        polyanya::Point point{0.0, 0.0};
        bool canonicalPortal{false};
    };
    std::vector<std::vector<polyanya::Point>> splitCells(cells.size());
    for (std::size_t cellIndex = 0u; cellIndex < cells.size(); ++cellIndex) {
        const NavRuntimeCell& cell = cells[cellIndex];
        if (cell.verticesXZ.size() < 3u) {
            return fail("Polyanya encountered a cell with fewer than three vertices");
        }
        std::vector<polyanya::Point>& split = splitCells[cellIndex];
        for (std::size_t edgeIndex = 0u;
             edgeIndex < cell.verticesXZ.size();
             ++edgeIndex) {
            const glm::vec2& sourceA = cell.verticesXZ[edgeIndex];
            const glm::vec2& sourceB =
                cell.verticesXZ[(edgeIndex + 1u) % cell.verticesXZ.size()];
            const polyanya::Point a{sourceA.x, sourceA.y};
            const polyanya::Point b{sourceB.x, sourceB.y};
            if (a.distance(b) <= kTopologyEpsilon) {
                return fail("Polyanya encountered a degenerate cell edge");
            }

            std::vector<EdgeSplitPoint> splitPoints{
                EdgeSplitPoint{0.0, a, false},
                EdgeSplitPoint{1.0, b, false},
            };
            for (const polyanya::Point& portalPoint :
                 portalPointsByComponent[components[cellIndex]]) {
                const bool belongsToCellPortal = std::any_of(
                    portalPointsByCell[cellIndex].begin(),
                    portalPointsByCell[cellIndex].end(),
                    [&](const polyanya::Point& localPoint) {
                        return localPoint == portalPoint;
                    }
                );
                const bool snapsToA = portalPoint.distance(a) <=
                    kJunctionSnapEpsilon;
                const bool snapsToB = portalPoint.distance(b) <=
                    kJunctionSnapEpsilon;
                // Interior points use the strict weld tolerance. Endpoints
                // are snapped to a shared junction because independent float
                // clipping can otherwise leave overlapping sub-millimetric
                // portal tails around a T-junction.
                if (!snapsToA && !snapsToB &&
                    !pointOnSegment(
                        portalPoint,
                        a,
                        b,
                        belongsToCellPortal
                            ? kJunctionSnapEpsilon
                            : kWeldEpsilon)) {
                    continue;
                }
                splitPoints.push_back(EdgeSplitPoint{
                    snapsToA ? 0.0 : snapsToB ? 1.0 : std::clamp(
                        segmentParameter(portalPoint, a, b), 0.0, 1.0),
                    portalPoint,
                    true,
                });
            }
            std::sort(
                splitPoints.begin(),
                splitPoints.end(),
                [](const EdgeSplitPoint& lhs, const EdgeSplitPoint& rhs) {
                    return lhs.parameter < rhs.parameter;
                }
            );
            const double edgeLength = a.distance(b);
            std::vector<EdgeSplitPoint> mergedSplitPoints{};
            mergedSplitPoints.reserve(splitPoints.size());
            for (const EdgeSplitPoint& candidate : splitPoints) {
                if (!mergedSplitPoints.empty() &&
                    std::abs(
                        candidate.parameter -
                        mergedSplitPoints.back().parameter
                    ) * edgeLength <= kJunctionSnapEpsilon) {
                    if (candidate.canonicalPortal &&
                        !mergedSplitPoints.back().canonicalPortal) {
                        mergedSplitPoints.back() = candidate;
                    }
                    continue;
                }
                mergedSplitPoints.push_back(candidate);
            }
            for (std::size_t parameterIndex = 0u;
                 parameterIndex + 1u < mergedSplitPoints.size();
                 ++parameterIndex) {
                const polyanya::Point point =
                    mergedSplitPoints[parameterIndex].point;
                if (split.empty() || !samePoint(split.back(), point)) {
                    split.push_back(point);
                }
            }
        }
        if (split.size() >= 2u && samePoint(split.front(), split.back())) {
            split.pop_back();
        }
        if (split.size() < 3u || signedArea(split) <= kTopologyEpsilon) {
            std::ostringstream message;
            message << "Polyanya cell " << cellIndex
                    << " is not a non-degenerate CCW polygon";
            return fail(message.str());
        }
        if (!isConvexCounterClockwise(split)) {
            std::ostringstream message;
            message << "Polyanya cell " << cellIndex
                    << " is not convex";
            return fail(message.str());
        }
    }

    auto result = std::make_shared<PolyanyaMesh>();
    result->mesh.max_poly_sides = 0;
    result->elevations.reserve(cells.size());
    result->components = components;
    result->mesh.mesh_polygons.resize(cells.size());
    std::unordered_map<WeldKey, std::vector<int>, WeldKeyHash>
        weldedVertices{};
    for (std::size_t cellIndex = 0u; cellIndex < splitCells.size(); ++cellIndex) {
        polyanya::Polygon& polygon = result->mesh.mesh_polygons[cellIndex];
        polygon.is_one_way = false;
        polygon.min_x = std::numeric_limits<double>::max();
        polygon.max_x = std::numeric_limits<double>::lowest();
        polygon.min_y = std::numeric_limits<double>::max();
        polygon.max_y = std::numeric_limits<double>::lowest();
        for (const polyanya::Point& point : splitCells[cellIndex]) {
            const std::int64_t bucketX = static_cast<std::int64_t>(
                std::floor(point.x / kWeldQuantum)
            );
            const std::int64_t bucketY = static_cast<std::int64_t>(
                std::floor(point.y / kWeldQuantum)
            );
            int weldedVertex = -1;
            double weldedDistance = std::numeric_limits<double>::max();
            for (std::int64_t offsetX = -1; offsetX <= 1; ++offsetX) {
                for (std::int64_t offsetY = -1; offsetY <= 1; ++offsetY) {
                    const auto bucket = weldedVertices.find(WeldKey{
                        components[cellIndex],
                        bucketX + offsetX,
                        bucketY + offsetY,
                    });
                    if (bucket == weldedVertices.end()) {
                        continue;
                    }
                    for (int candidate : bucket->second) {
                        const double distance = result->mesh.mesh_vertices[
                            static_cast<std::size_t>(candidate)
                        ].p.distance(point);
                        if (distance <= kWeldEpsilon &&
                            distance < weldedDistance) {
                            weldedDistance = distance;
                            weldedVertex = candidate;
                        }
                    }
                }
            }
            if (weldedVertex < 0) {
                weldedVertex = static_cast<int>(
                    result->mesh.mesh_vertices.size()
                );
                result->mesh.mesh_vertices.push_back(polyanya::Vertex{
                    point,
                    {},
                    false,
                    false,
                });
                weldedVertices[WeldKey{
                    components[cellIndex],
                    bucketX,
                    bucketY,
                }].push_back(weldedVertex);
            }
            polygon.vertices.push_back(weldedVertex);
            result->mesh.mesh_vertices[
                static_cast<std::size_t>(weldedVertex)
            ]
                .polygons.push_back(static_cast<int>(cellIndex));
            polygon.min_x = std::min(polygon.min_x, point.x);
            polygon.max_x = std::max(polygon.max_x, point.x);
            polygon.min_y = std::min(polygon.min_y, point.y);
            polygon.max_y = std::max(polygon.max_y, point.y);
        }
        polygon.polygons.assign(polygon.vertices.size(), -1);
        result->mesh.max_poly_sides = std::max(
            result->mesh.max_poly_sides,
            static_cast<int>(polygon.vertices.size()));
        result->elevations.push_back(cells[cellIndex].elevationY);
    }

    std::unordered_map<
        UndirectedEdgeKey,
        std::vector<EdgeOccurrence>,
        UndirectedEdgeKeyHash
    > occurrences{};
    for (std::size_t cellIndex = 0u;
         cellIndex < result->mesh.mesh_polygons.size();
         ++cellIndex) {
        const polyanya::Polygon& polygon =
            result->mesh.mesh_polygons[cellIndex];
        for (std::size_t slot = 0u; slot < polygon.vertices.size(); ++slot) {
            const int current = polygon.vertices[slot];
            const int previous = polygon.vertices[
                (slot + polygon.vertices.size() - 1u) % polygon.vertices.size()];
            if (current == previous) {
                return fail("Polyanya welding produced a zero-length edge");
            }
            std::vector<EdgeOccurrence>& edgeOccurrences =
                occurrences[UndirectedEdgeKey{
                std::min(current, previous),
                std::max(current, previous),
            }];
            if (std::none_of(
                    edgeOccurrences.begin(),
                    edgeOccurrences.end(),
                    [&](const EdgeOccurrence& occurrence) {
                        return occurrence.cell == cellIndex;
                    })) {
                edgeOccurrences.push_back(EdgeOccurrence{cellIndex, slot});
            }
        }
    }

    std::vector<std::vector<std::uint8_t>> matchedPortals(graph.size());
    for (std::size_t cellIndex = 0u; cellIndex < graph.size(); ++cellIndex) {
        matchedPortals[cellIndex].assign(graph[cellIndex].size(), 0u);
    }

    for (const auto& [edgeKey, edgeOccurrences] : occurrences) {
        const polyanya::Point midpoint =
            (result->mesh.mesh_vertices[static_cast<std::size_t>(edgeKey.low)].p +
             result->mesh.mesh_vertices[static_cast<std::size_t>(edgeKey.high)].p) *
            0.5;
        std::vector<std::uint8_t> paired(edgeOccurrences.size(), 0u);
        for (std::size_t lhsIndex = 0u;
             lhsIndex < edgeOccurrences.size();
             ++lhsIndex) {
            for (std::size_t rhsIndex = lhsIndex + 1u;
                 rhsIndex < edgeOccurrences.size();
                 ++rhsIndex) {
                const EdgeOccurrence lhs = edgeOccurrences[lhsIndex];
                const EdgeOccurrence rhs = edgeOccurrences[rhsIndex];
                if (lhs.cell == rhs.cell ||
                    !graphContainsPortal(
                        graph,
                        lhs.cell,
                        rhs.cell,
                        midpoint,
                        nullptr) ||
                    !graphContainsPortal(
                        graph,
                        rhs.cell,
                        lhs.cell,
                        midpoint,
                        nullptr)) {
                    continue;
                }
                if (paired[lhsIndex] != 0u || paired[rhsIndex] != 0u) {
                    std::ostringstream message;
                    message << "Polyanya graph has multiple neighbours on "
                            << "one welded polygon edge while pairing "
                            << lhs.cell << " and " << rhs.cell
                            << " from ("
                            << result->mesh.mesh_vertices[
                                static_cast<std::size_t>(edgeKey.low)
                            ].p.x << ", "
                            << result->mesh.mesh_vertices[
                                static_cast<std::size_t>(edgeKey.low)
                            ].p.y << ") to ("
                            << result->mesh.mesh_vertices[
                                static_cast<std::size_t>(edgeKey.high)
                            ].p.x << ", "
                            << result->mesh.mesh_vertices[
                                static_cast<std::size_t>(edgeKey.high)
                            ].p.y << ") (occurrence cells";
                    for (const EdgeOccurrence& occurrence : edgeOccurrences) {
                        message << " " << occurrence.cell;
                    }
                    message << ")";
                    return fail(message.str());
                }
                graphContainsPortal(
                    graph,
                    lhs.cell,
                    rhs.cell,
                    midpoint,
                    &matchedPortals
                );
                graphContainsPortal(
                    graph,
                    rhs.cell,
                    lhs.cell,
                    midpoint,
                    &matchedPortals
                );
                result->mesh.mesh_polygons[lhs.cell]
                    .polygons[lhs.neighbourSlot] =
                        static_cast<int>(rhs.cell);
                result->mesh.mesh_polygons[rhs.cell]
                    .polygons[rhs.neighbourSlot] =
                        static_cast<int>(lhs.cell);
                paired[lhsIndex] = 1u;
                paired[rhsIndex] = 1u;
            }
        }
        if (std::any_of(
                paired.begin(),
                paired.end(),
                [](std::uint8_t value) { return value == 0u; })) {
            // Coincident quantized boundaries without a graph transition stay
            // closed. Intended transitions were paired explicitly above.
            result->mesh.mesh_vertices[static_cast<std::size_t>(edgeKey.low)]
                .is_corner = true;
            result->mesh.mesh_vertices[static_cast<std::size_t>(edgeKey.high)]
                .is_corner = true;
        }
    }

    for (std::size_t cellIndex = 0u; cellIndex < graph.size(); ++cellIndex) {
        for (std::size_t edgeIndex = 0u;
             edgeIndex < graph[cellIndex].size();
             ++edgeIndex) {
            if (!graph[cellIndex][edgeIndex].viaLink &&
                matchedPortals[cellIndex][edgeIndex] == 0u) {
                const NavGraphEdge& edge = graph[cellIndex][edgeIndex];
                std::ostringstream message;
                message << "Polyanya graph portal " << cellIndex << " -> "
                        << edge.targetCellIndex << " from ("
                        << edge.portalA.x << ", " << edge.portalA.y
                        << ") to (" << edge.portalB.x << ", "
                        << edge.portalB.y
                        << ") has no reciprocal welded mesh edge";
                return fail(message.str());
            }
        }
    }

    if (error != nullptr) {
        error->clear();
    }
    return result;
}

std::optional<PolyanyaPath> findPolyanyaPath(
    const PolyanyaMesh& polyanyaMesh,
    const glm::vec3& start3,
    const glm::vec3& destination3,
    const std::vector<std::size_t>& startCells,
    const std::vector<std::size_t>& targetCells,
    const std::atomic<bool>* cancelled
) {
    const polyanya::Mesh& mesh = polyanyaMesh.mesh;
    if (mesh.mesh_polygons.empty() || startCells.empty() ||
        targetCells.empty() ||
        polyanyaMesh.components.size() != mesh.mesh_polygons.size()) {
        return std::nullopt;
    }
    const auto isCancelled = [&]() {
        return cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed);
    };
    const polyanya::Point start{start3.x, start3.z};
    const polyanya::Point goal{destination3.x, destination3.z};

    std::vector<std::uint8_t> targetComponents{};
    for (std::size_t targetCell : targetCells) {
        if (targetCell >= polyanyaMesh.components.size()) {
            continue;
        }
        const std::size_t component =
            polyanyaMesh.components[targetCell];
        if (component >= targetComponents.size()) {
            targetComponents.resize(component + 1u, 0u);
        }
        targetComponents[component] = 1u;
    }
    bool sharesPlanarComponent = false;
    for (std::size_t startCell : startCells) {
        if (startCell >= polyanyaMesh.components.size()) {
            continue;
        }
        const std::size_t component =
            polyanyaMesh.components[startCell];
        if (component < targetComponents.size() &&
            targetComponents[component] != 0u) {
            sharesPlanarComponent = true;
            break;
        }
    }
    if (!sharesPlanarComponent) {
        return std::nullopt;
    }
    if (start == goal) {
        return PolyanyaPath{{destination3}, 0.0, 0u};
    }

    std::vector<std::uint8_t> starts(mesh.mesh_polygons.size(), 0u);
    std::vector<std::uint8_t> targets(mesh.mesh_polygons.size(), 0u);
    for (std::size_t cell : startCells) {
        if (cell < starts.size() &&
            polyanyaMesh.components[cell] < targetComponents.size() &&
            targetComponents[polyanyaMesh.components[cell]] != 0u) {
            starts[cell] = 1u;
        }
    }
    for (std::size_t cell : targetCells) {
        if (cell < targets.size() &&
            polyanyaMesh.components[cell] < targetComponents.size() &&
            targetComponents[polyanyaMesh.components[cell]] != 0u) {
            targets[cell] = 1u;
            if (starts[cell] != 0u) {
                return PolyanyaPath{
                    {destination3},
                    static_cast<double>(glm::distance(start3, destination3)),
                    0u,
                };
            }
        }
    }

    std::deque<SearchNode> storage{};
    std::priority_queue<
        SearchNode*,
        std::vector<SearchNode*>,
        SearchNodeCompare
    > open{};
    std::vector<double> bestRootG(
        mesh.mesh_vertices.size(),
        std::numeric_limits<double>::infinity());
    bool invalidIntervalEncountered = false;

    const auto pushNode = [&](polyanya::SearchNode interval,
                              SearchNode* parent) {
        const polyanya::Point root =
            rootPoint(mesh, interval.root, start);
        // Polyanya's expansion code requires right to lie clockwise from left
        // as viewed from the root. The endpoint vertex order is tied to the
        // entered polygon, so an invalid interval cannot safely be repaired by
        // swapping it. Reject it and let another interval (or the validated A*
        // fallback) handle the query instead of reaching a reference assert.
        if (cross(interval.left - root, interval.right - root) >
            kSearchEpsilon) {
            invalidIntervalEncountered = true;
            return;
        }
        if (interval.root >= 0) {
            const std::size_t root = static_cast<std::size_t>(interval.root);
            if (root >= bestRootG.size() ||
                interval.g > bestRootG[root] + kSearchEpsilon) {
                return;
            }
            bestRootG[root] = std::min(bestRootG[root], interval.g);
        }
        interval.f = interval.g +
            polyanya::get_h_value(root, goal, interval.left, interval.right);
        storage.push_back(SearchNode{interval, parent});
        open.push(&storage.back());
    };

    for (std::size_t startCell : startCells) {
        if (startCell >= mesh.mesh_polygons.size()) {
            continue;
        }
        const polyanya::Polygon& polygon = mesh.mesh_polygons[startCell];
        for (std::size_t slot = 0u; slot < polygon.vertices.size(); ++slot) {
            const int nextPolygon = polygon.polygons[slot];
            if (nextPolygon < 0 ||
                starts[static_cast<std::size_t>(nextPolygon)] != 0u) {
                continue;
            }
            const int leftVertex = polygon.vertices[slot];
            const int rightVertex = polygon.vertices[
                (slot + polygon.vertices.size() - 1u) % polygon.vertices.size()];
            pushNode(polyanya::SearchNode{
                nullptr,
                -1,
                mesh.mesh_vertices[static_cast<std::size_t>(leftVertex)].p,
                mesh.mesh_vertices[static_cast<std::size_t>(rightVertex)].p,
                leftVertex,
                rightVertex,
                nextPolygon,
                0.0,
                0.0,
            }, nullptr);
        }
    }
    if (invalidIntervalEncountered) {
        return std::nullopt;
    }
    if (open.empty()) {
        return std::nullopt;
    }

    std::vector<polyanya::Successor> successors(
        static_cast<std::size_t>(mesh.max_poly_sides + 2));
    std::size_t expanded = 0u;
    const std::size_t maximumExpandedNodes = std::max<std::size_t>(
        4096u,
        mesh.mesh_polygons.size() * 8u
    );
    SearchNode* finalNode = nullptr;
    int finalRoot = -1;
    while (!open.empty()) {
        if ((expanded & 31u) == 0u && isCancelled()) {
            return std::nullopt;
        }
        // A conforming convex mesh normally expands only a small multiple of
        // its polygon count. Degenerate snapped intervals can otherwise keep
        // generating equivalent states indefinitely. Abort the exact attempt
        // at a generous deterministic bound; the caller immediately retries
        // through the validated A* + funnel pipeline.
        if (expanded >= maximumExpandedNodes) {
            return std::nullopt;
        }
        SearchNode* node = open.top();
        open.pop();
        ++expanded;
        if (node->interval.root >= 0) {
            const std::size_t root =
                static_cast<std::size_t>(node->interval.root);
            if (node->interval.g >
                bestRootG[root] + kSearchEpsilon) {
                continue;
            }
        }
        if (node->interval.next_polygon < 0 ||
            static_cast<std::size_t>(node->interval.next_polygon) >=
                targets.size()) {
            continue;
        }
        if (targets[static_cast<std::size_t>(node->interval.next_polygon)] != 0u) {
            const polyanya::Point root =
                rootPoint(mesh, node->interval.root, start);
            const polyanya::Point rootGoal = goal - root;
            if (rootGoal * (node->interval.left - root) <
                -kSearchEpsilon) {
                finalRoot = node->interval.left_vertex;
            } else if ((node->interval.right - root) * rootGoal <
                       -kSearchEpsilon) {
                finalRoot = node->interval.right_vertex;
            } else {
                finalRoot = node->interval.root;
            }
            finalNode = node;
            break;
        }

        const int successorCount = polyanya::get_successors(
            node->interval,
            start,
            mesh,
            successors.data());
        const polyanya::Polygon& polygon = mesh.mesh_polygons[
            static_cast<std::size_t>(node->interval.next_polygon)];
        const polyanya::Point parentRoot =
            rootPoint(mesh, node->interval.root, start);
        double leftG = -1.0;
        double rightG = -1.0;
        for (int successorIndex = 0;
             successorIndex < successorCount;
             ++successorIndex) {
            const polyanya::Successor& successor =
                successors[static_cast<std::size_t>(successorIndex)];
            const int slot = successor.poly_left_ind;
            if (slot < 0 || static_cast<std::size_t>(slot) >=
                    polygon.vertices.size()) {
                continue;
            }
            const int nextPolygon =
                polygon.polygons[static_cast<std::size_t>(slot)];
            if (nextPolygon < 0) {
                continue;
            }
            const int leftVertex =
                polygon.vertices[static_cast<std::size_t>(slot)];
            const int rightVertex = polygon.vertices[
                slot == 0 ? polygon.vertices.size() - 1u
                          : static_cast<std::size_t>(slot - 1)];
            int root = node->interval.root;
            double g = node->interval.g;
            switch (successor.type) {
                case polyanya::Successor::RIGHT_NON_OBSERVABLE:
                    if (rightG < 0.0) {
                        rightG = node->interval.g +
                            parentRoot.distance(node->interval.right);
                    }
                    root = node->interval.right_vertex;
                    g = rightG;
                    break;
                case polyanya::Successor::OBSERVABLE:
                    break;
                case polyanya::Successor::LEFT_NON_OBSERVABLE:
                    if (leftG < 0.0) {
                        leftG = node->interval.g +
                            parentRoot.distance(node->interval.left);
                    }
                    root = node->interval.left_vertex;
                    g = leftG;
                    break;
            }
            pushNode(polyanya::SearchNode{
                nullptr,
                root,
                successor.left,
                successor.right,
                leftVertex,
                rightVertex,
                nextPolygon,
                g,
                g,
            }, node);
            if (invalidIntervalEncountered) {
                return std::nullopt;
            }
        }
    }
    if (finalNode == nullptr || isCancelled()) {
        return std::nullopt;
    }

    std::vector<polyanya::Point> reversed{goal};
    if (finalRoot >= 0) {
        const polyanya::Point root =
            mesh.mesh_vertices[static_cast<std::size_t>(finalRoot)].p;
        if (!samePoint(root, reversed.back())) {
            reversed.push_back(root);
        }
    } else if (!samePoint(start, reversed.back())) {
        reversed.push_back(start);
    }
    for (SearchNode* node = finalNode; node != nullptr; node = node->parent) {
        const polyanya::Point root =
            rootPoint(mesh, node->interval.root, start);
        if (!samePoint(root, reversed.back())) {
            reversed.push_back(root);
        }
    }
    if (!samePoint(start, reversed.back())) {
        reversed.push_back(start);
    }
    std::reverse(reversed.begin(), reversed.end());

    PolyanyaPath path{};
    path.expandedNodes = expanded;
    glm::vec3 previous = start3;
    for (std::size_t index = 1u; index < reversed.size(); ++index) {
        const bool destination = index + 1u == reversed.size();
        const glm::vec3 corner = destination
            ? destination3
            : glm::vec3(
                static_cast<float>(reversed[index].x),
                start3.y,
                static_cast<float>(reversed[index].y));
        if (!path.corners.empty() &&
            glm::distance(path.corners.back(), corner) <=
                static_cast<float>(kSearchEpsilon)) {
            continue;
        }
        path.length += static_cast<double>(glm::distance(previous, corner));
        path.corners.push_back(corner);
        previous = corner;
    }
    if (path.corners.empty() ||
        glm::distance(path.corners.back(), destination3) >
            static_cast<float>(kSearchEpsilon)) {
        path.length += static_cast<double>(glm::distance(previous, destination3));
        path.corners.push_back(destination3);
    }
    return path;
}

} // namespace core::navigation_detail
