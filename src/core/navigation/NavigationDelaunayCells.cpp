#include "core/navigation/NavigationDetailBake.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"

#include "CDT.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace core::navigation_detail {
namespace {

void setError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool triangleIsInsideConvexCell(
    const std::vector<glm::vec2>& triangle,
    const std::vector<glm::vec2>& cell
) {
    for (const glm::vec2& vertex : triangle) {
        if (!pointInOrOnPolygonXZ(vertex, cell)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<std::vector<NavRuntimeCell>> triangulateWalkableCellsIndividuallyDelaunay(
    const std::vector<NavRuntimeCell>& cells,
    float elevationY,
    std::string* error
) {
    std::vector<NavRuntimeCell> triangles{};
    for (const NavRuntimeCell& sourceCell : cells) {
        const std::vector<glm::vec2> cell =
            normalizePolygonVertices(sourceCell.verticesXZ);
        if (!polygonHasArea(cell)) {
            setError(error, "a source cell is degenerate");
            return std::nullopt;
        }

        std::vector<CDT::V2d<double>> vertices{};
        std::vector<CDT::Edge> constraints{};
        vertices.reserve(cell.size());
        constraints.reserve(cell.size());
        for (std::size_t index = 0u; index < cell.size(); ++index) {
            const glm::vec2& point = cell[index];
            vertices.push_back(CDT::V2d<double>{point.x, point.y});
            constraints.emplace_back(
                static_cast<CDT::VertInd>(index),
                static_cast<CDT::VertInd>((index + 1u) % cell.size())
            );
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

            double outputArea = 0.0;
            for (const CDT::Triangle& triangle : triangulation.triangles) {
                std::vector<glm::vec2> points{};
                points.reserve(3u);
                for (const CDT::VertInd index : triangle.vertices) {
                    if (index >= triangulation.vertices.size()) {
                        setError(error, "CDT returned an invalid vertex index");
                        return std::nullopt;
                    }
                    const CDT::V2d<double>& vertex = triangulation.vertices[index];
                    points.emplace_back(
                        static_cast<float>(vertex.x),
                        static_cast<float>(vertex.y)
                    );
                }
                points = normalizePolygonVertices(points);
                if (!polygonHasArea(points) || !triangleIsInsideConvexCell(points, cell)) {
                    setError(error, "a triangle leaves its source cell");
                    return std::nullopt;
                }
                outputArea += std::abs(static_cast<double>(polygonSignedArea(points)));
                triangles.push_back(NavRuntimeCell{elevationY, std::move(points)});
            }
            const double inputArea = std::abs(
                static_cast<double>(polygonSignedArea(cell))
            );
            const double tolerance = std::max(
                static_cast<double>(kPolygonEpsilon) * 32.0,
                inputArea * 1.0e-4
            );
            if (std::abs(inputArea - outputArea) > tolerance) {
                setError(error, "the cell output area is incomplete");
                return std::nullopt;
            }
        } catch (const std::exception& exception) {
            setError(error, "CDT error: " + std::string(exception.what()));
            return std::nullopt;
        }
    }
    return triangles.empty() ? std::nullopt
                             : std::optional<std::vector<NavRuntimeCell>>{
                                   std::move(triangles)};
}

std::vector<NavRuntimeCell> applyMinimumTriangleAreaConstraint(
    const std::vector<NavRuntimeCell>& triangles,
    float minimumTriangleArea
) {
    if (minimumTriangleArea <= 0.0f) {
        return triangles;
    }

    std::vector<NavRuntimeCell> retained{};
    retained.reserve(triangles.size());
    for (const NavRuntimeCell& triangle : triangles) {
        const float area = std::abs(polygonSignedArea(triangle.verticesXZ));
        if (area + kPolygonEpsilon >= minimumTriangleArea) {
            retained.push_back(triangle);
        }
    }
    return retained;
}

}  // namespace core::navigation_detail
