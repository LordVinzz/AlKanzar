#include "Polyanya.hpp"
#include "PolyanyaTopology.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core::navigation_detail {

namespace {

bool samePoint(const polyanya::Point& lhs, const polyanya::Point& rhs) {
    return lhs == rhs;
}

}  // namespace

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

}  // namespace core::navigation_detail
