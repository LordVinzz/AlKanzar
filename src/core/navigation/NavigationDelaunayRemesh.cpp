#include "core/navigation/NavigationDetailBake.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"

#include "CDT.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <optional>
#include <unordered_map>
#include <vector>

namespace core::navigation_detail {
namespace {

struct DirectedBoundarySegment {
    QuantizedVec2 fromKey{};
    QuantizedVec2 toKey{};
    glm::vec2 from{0.0f};
    glm::vec2 to{0.0f};
};

struct UndirectedBoundaryEdge {
    QuantizedVec2 first{};
    QuantizedVec2 second{};

    friend bool operator==(const UndirectedBoundaryEdge&, const UndirectedBoundaryEdge&) = default;
};

struct UndirectedBoundaryEdgeHash {
    std::size_t operator()(const UndirectedBoundaryEdge& edge) const noexcept {
        const std::size_t first = QuantizedVec2Hash{}(edge.first);
        return first ^ (QuantizedVec2Hash{}(edge.second) + 0x9e3779b9u +
                        (first << 6u) + (first >> 2u));
    }
};

struct CountedBoundarySegment {
    DirectedBoundarySegment segment{};
    std::size_t count{0u};
};

UndirectedBoundaryEdge canonicalBoundaryEdge(QuantizedVec2 from, QuantizedVec2 to) {
    if (to < from) {
        std::swap(from, to);
    }
    return UndirectedBoundaryEdge{from, to};
}

void addUniqueSplitVertex(
    std::vector<std::pair<float, QuantizedVec2>>& vertices,
    float parameter,
    QuantizedVec2 key
) {
    const auto duplicate = std::find_if(
        vertices.begin(), vertices.end(),
        [key](const std::pair<float, QuantizedVec2>& entry) {
            return entry.second == key;
        }
    );
    if (duplicate != vertices.end()) {
        return;
    }
    vertices.emplace_back(parameter, key);
}

std::optional<std::vector<DirectedBoundarySegment>> extractBoundarySegments(
    const std::vector<NavRuntimeCell>& cells
) {
    std::unordered_map<
        QuantizedVec2,
        glm::vec2,
        QuantizedVec2Hash
    > vertices{};
    std::vector<DirectedBoundarySegment> sourceEdges{};
    for (const NavRuntimeCell& cell : cells) {
        std::vector<glm::vec2> polygon = normalizePolygonVertices(cell.verticesXZ);
        if (!polygonHasArea(polygon)) {
            continue;
        }
        for (const glm::vec2& point : polygon) {
            vertices.try_emplace(quantizeVec2(point), point);
        }
        for (std::size_t index = 0u; index < polygon.size(); ++index) {
            const glm::vec2& from = polygon[index];
            const glm::vec2& to = polygon[(index + 1u) % polygon.size()];
            if (glm::distance(from, to) <= kPolygonEpsilon) {
                continue;
            }
            sourceEdges.push_back(DirectedBoundarySegment{
                quantizeVec2(from),
                quantizeVec2(to),
                from,
                to,
            });
        }
    }
    if (sourceEdges.empty()) {
        return std::nullopt;
    }

    std::unordered_map<
        UndirectedBoundaryEdge,
        CountedBoundarySegment,
        UndirectedBoundaryEdgeHash
    > countedEdges{};
    for (const DirectedBoundarySegment& edge : sourceEdges) {
        const glm::vec2 direction = edge.to - edge.from;
        const float lengthSquared = glm::dot(direction, direction);
        if (lengthSquared <= kPolygonEpsilon * kPolygonEpsilon) {
            continue;
        }

        std::vector<std::pair<float, QuantizedVec2>> splitVertices{};
        splitVertices.reserve(vertices.size());
        for (const auto& [key, point] : vertices) {
            if (!pointOnSegmentXZ(point, edge.from, edge.to)) {
                continue;
            }
            const float parameter = std::clamp(
                glm::dot(point - edge.from, direction) / lengthSquared,
                0.0f,
                1.0f
            );
            addUniqueSplitVertex(splitVertices, parameter, key);
        }
        std::sort(
            splitVertices.begin(), splitVertices.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.first < rhs.first;
            }
        );
        for (std::size_t index = 1u; index < splitVertices.size(); ++index) {
            const QuantizedVec2 fromKey = splitVertices[index - 1u].second;
            const QuantizedVec2 toKey = splitVertices[index].second;
            if (fromKey == toKey) {
                continue;
            }
            const auto fromIt = vertices.find(fromKey);
            const auto toIt = vertices.find(toKey);
            if (fromIt == vertices.end() || toIt == vertices.end() ||
                glm::distance(fromIt->second, toIt->second) <= kPolygonEpsilon) {
                continue;
            }
            CountedBoundarySegment& counted = countedEdges[
                canonicalBoundaryEdge(fromKey, toKey)
            ];
            if (counted.count == 0u) {
                counted.segment = DirectedBoundarySegment{
                    fromKey,
                    toKey,
                    fromIt->second,
                    toIt->second,
                };
            }
            ++counted.count;
        }
    }

    std::vector<DirectedBoundarySegment> boundary{};
    boundary.reserve(countedEdges.size());
    for (const auto& entry : countedEdges) {
        const CountedBoundarySegment& counted = entry.second;
        if (counted.count > 2u) {
            return std::nullopt;
        }
        if (counted.count == 1u) {
            boundary.push_back(counted.segment);
        }
    }
    return boundary.empty() ? std::nullopt
                            : std::optional<std::vector<DirectedBoundarySegment>>{
                                  std::move(boundary)};
}

bool hasExpectedArea(
    const std::vector<NavRuntimeCell>& sourceCells,
    const std::vector<NavRuntimeCell>& triangles
) {
    double sourceArea = 0.0;
    for (const NavRuntimeCell& cell : sourceCells) {
        sourceArea += std::abs(static_cast<double>(polygonSignedArea(cell.verticesXZ)));
    }
    double triangleArea = 0.0;
    for (const NavRuntimeCell& triangle : triangles) {
        triangleArea += std::abs(static_cast<double>(polygonSignedArea(triangle.verticesXZ)));
    }
    const double tolerance = std::max(
        static_cast<double>(kPolygonEpsilon) * 32.0,
        sourceArea * 1.0e-4
    );
    return !triangles.empty() && std::abs(sourceArea - triangleArea) <= tolerance;
}

bool triangleIsCoveredBySourceCells(
    const std::vector<glm::vec2>& triangle,
    const std::vector<NavRuntimeCell>& sourceCells
) {
    const float triangleArea = std::abs(polygonSignedArea(triangle));
    if (triangleArea <= kPolygonEpsilon) {
        return false;
    }
    const auto [triangleMinXZ, triangleMaxXZ] = polygonBoundsXZ(triangle);
    std::vector<std::vector<glm::vec2>> uncovered{triangle};
    for (const NavRuntimeCell& sourceCell : sourceCells) {
        if (uncovered.empty()) {
            break;
        }
        const auto [cellMinXZ, cellMaxXZ] =
            polygonBoundsXZ(sourceCell.verticesXZ);
        if (!boundsOverlapXZ(
                triangleMinXZ,
                triangleMaxXZ,
                cellMinXZ,
                cellMaxXZ)) {
            continue;
        }
        std::vector<std::vector<glm::vec2>> remaining{};
        for (const std::vector<glm::vec2>& piece : uncovered) {
            std::vector<std::vector<glm::vec2>> outside =
                subtractConvexPolygon(
                    piece,
                    sourceCell.verticesXZ,
                    cellMinXZ,
                    cellMaxXZ
                );
            remaining.insert(
                remaining.end(),
                std::make_move_iterator(outside.begin()),
                std::make_move_iterator(outside.end())
            );
        }
        uncovered = std::move(remaining);
    }

    double uncoveredArea = 0.0;
    const double tolerance = std::max(
        static_cast<double>(kPolygonEpsilon) * 32.0,
        static_cast<double>(triangleArea) * 1.0e-4
    );
    for (const std::vector<glm::vec2>& piece : uncovered) {
        uncoveredArea += std::abs(
            static_cast<double>(polygonSignedArea(piece))
        );
        if (uncoveredArea > tolerance) {
            return false;
        }
    }
    return true;
}

std::size_t longestTriangleEdgeIndex(const std::vector<glm::vec2>& triangle) {
    std::size_t result = 0u;
    float longestLengthSquared = 0.0f;
    for (std::size_t index = 0u; index < triangle.size(); ++index) {
        const glm::vec2& from = triangle[index];
        const glm::vec2& to = triangle[(index + 1u) % triangle.size()];
        const float lengthSquared = glm::dot(to - from, to - from);
        if (lengthSquared > longestLengthSquared) {
            longestLengthSquared = lengthSquared;
            result = index;
        }
    }
    return result;
}

bool triangleFitsMaximumEdgeLength(
    const std::vector<glm::vec2>& triangle,
    float maximumEdgeLength
) {
    const std::size_t edgeIndex = longestTriangleEdgeIndex(triangle);
    const glm::vec2& from = triangle[edgeIndex];
    const glm::vec2& to = triangle[(edgeIndex + 1u) % triangle.size()];
    return glm::distance(from, to) <= maximumEdgeLength + kPolygonEpsilon;
}

void setDelaunayError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

}  // namespace

std::optional<std::vector<NavRuntimeCell>> triangulateWalkableCellsGloballyDelaunay(
    const std::vector<NavRuntimeCell>& cells,
    float elevationY,
    std::string* error
) {
    const std::optional<std::vector<DirectedBoundarySegment>> boundary =
        extractBoundarySegments(cells);
    if (!boundary.has_value()) {
        setDelaunayError(error, "the walkable union has no closed, manifold boundary");
        return std::nullopt;
    }

    std::unordered_map<QuantizedVec2, CDT::VertInd, QuantizedVec2Hash> indices{};
    std::vector<CDT::V2d<double>> vertices{};
    std::vector<CDT::Edge> constraints{};
    const auto indexFor = [&](QuantizedVec2 key, const glm::vec2& point) {
        const auto found = indices.find(key);
        if (found != indices.end()) {
            return found->second;
        }
        const CDT::VertInd index = static_cast<CDT::VertInd>(vertices.size());
        indices.emplace(key, index);
        vertices.push_back(CDT::V2d<double>{
            static_cast<double>(point.x),
            static_cast<double>(point.y),
        });
        return index;
    };
    for (const DirectedBoundarySegment& edge : *boundary) {
        const CDT::VertInd from = indexFor(edge.fromKey, edge.from);
        const CDT::VertInd to = indexFor(edge.toKey, edge.to);
        if (from != to) {
            constraints.push_back(CDT::Edge(from, to));
        }
    }
    if (vertices.size() < 3u || constraints.empty()) {
        setDelaunayError(error, "there are not enough valid constrained vertices");
        return std::nullopt;
    }

    try {
        CDT::Triangulation<double> triangulation(
            CDT::VertexInsertionOrder::Auto,
            CDT::IntersectingConstraintEdges::TryResolve,
            static_cast<double>(kPolygonEpsilon)
        );
        triangulation.insertVertices(vertices);
        triangulation.insertEdges(constraints);
        triangulation.eraseOuterTrianglesAndHoles();

        std::vector<NavRuntimeCell> triangles{};
        triangles.reserve(triangulation.triangles.size());
        for (const CDT::Triangle& triangle : triangulation.triangles) {
            std::vector<glm::vec2> points{};
            points.reserve(3u);
            for (const CDT::VertInd index : triangle.vertices) {
                if (index >= triangulation.vertices.size()) {
                    setDelaunayError(error, "CDT returned an invalid vertex index");
                    return std::nullopt;
                }
                const CDT::V2d<double>& vertex = triangulation.vertices[index];
                points.emplace_back(
                    static_cast<float>(vertex.x),
                    static_cast<float>(vertex.y)
                );
            }
            points = normalizePolygonVertices(points);
            if (!polygonHasArea(points)) {
                continue;
            }
            if (!triangleIsCoveredBySourceCells(points, cells)) {
                setDelaunayError(error, "a triangle would leave the walkable surface");
                return std::nullopt;
            }
            triangles.push_back(NavRuntimeCell{elevationY, std::move(points)});
        }
        if (!hasExpectedArea(cells, triangles)) {
            setDelaunayError(error, "the output area differs from the walkable surface");
            return std::nullopt;
        }
        return triangles;
    } catch (const std::exception& exception) {
        setDelaunayError(error, "CDT error: " + std::string(exception.what()));
        return std::nullopt;
    }
}

std::optional<std::vector<NavRuntimeCell>> triangulateWalkableCellsDelaunay(
    const std::vector<NavRuntimeCell>& cells,
    float elevationY,
    std::string* error
) {
    std::string globalError{};
    if (const auto globallyTriangulated =
            triangulateWalkableCellsGloballyDelaunay(
                cells,
                elevationY,
                &globalError
            );
        globallyTriangulated.has_value()) {
        return globallyTriangulated;
    }

    std::string individualError{};
    const auto individuallyTriangulated =
        triangulateWalkableCellsIndividuallyDelaunay(
            cells,
            elevationY,
            &individualError
        );
    if (individuallyTriangulated.has_value()) {
        return individuallyTriangulated;
    }
    setDelaunayError(
        error,
        "global pass failed (" + globalError + "); cell pass failed (" +
            individualError + ")"
    );
    return std::nullopt;
}

std::optional<std::vector<NavRuntimeCell>> enforceMaximumPolygonEdgeLength(
    const std::vector<NavRuntimeCell>& cells,
    float maximumPolygonEdgeLength
) {
    if (maximumPolygonEdgeLength <= 0.0f) {
        return cells;
    }

    std::vector<NavRuntimeCell> pending{};
    for (const NavRuntimeCell& cell : cells) {
        const auto cellTriangles = triangulateSimplePolygon(cell.verticesXZ);
        if (cellTriangles.empty()) {
            return std::nullopt;
        }
        for (const auto& triangle : cellTriangles) {
            pending.push_back(NavRuntimeCell{
                cell.elevationY,
                {triangle[0], triangle[1], triangle[2]}
            });
        }
    }
    if (pending.size() > kMaxGeneratedNavMeshCells) {
        return std::nullopt;
    }

    std::vector<NavRuntimeCell> constrained{};
    constrained.reserve(pending.size());
    while (!pending.empty()) {
        NavRuntimeCell triangle = std::move(pending.back());
        pending.pop_back();
        triangle.verticesXZ = normalizePolygonVertices(triangle.verticesXZ);
        if (!polygonHasArea(triangle.verticesXZ) ||
            triangle.verticesXZ.size() != 3u) {
            return std::nullopt;
        }
        if (triangleFitsMaximumEdgeLength(
                triangle.verticesXZ,
                maximumPolygonEdgeLength)) {
            constrained.push_back(std::move(triangle));
            continue;
        }
        if (constrained.size() + pending.size() + 2u >
            kMaxGeneratedNavMeshCells) {
            return std::nullopt;
        }

        const std::size_t edgeIndex =
            longestTriangleEdgeIndex(triangle.verticesXZ);
        const glm::vec2& from = triangle.verticesXZ[edgeIndex];
        const glm::vec2& to = triangle.verticesXZ[
            (edgeIndex + 1u) % triangle.verticesXZ.size()];
        const glm::vec2& opposite = triangle.verticesXZ[
            (edgeIndex + 2u) % triangle.verticesXZ.size()];
        const glm::vec2 midpoint = (from + to) * 0.5f;
        pending.push_back(NavRuntimeCell{
            triangle.elevationY,
            {from, midpoint, opposite}
        });
        pending.push_back(NavRuntimeCell{
            triangle.elevationY,
            {midpoint, to, opposite}
        });
    }
    return constrained;
}

}  // namespace core::navigation_detail
