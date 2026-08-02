#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"
#include "core/navigation/NavigationDetailSurface.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace core::navigation_detail {

void ensureCCW(std::vector<glm::vec2>& vertices) {
    if (vertices.size() < 3u) {
        return;
    }
    float twiceArea = 0.0f;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const std::size_t j = (i + 1u) % vertices.size();
        twiceArea += cross2(vertices[i], vertices[j]);
    }
    if (twiceArea < 0.0f) {
        std::reverse(vertices.begin(), vertices.end());
    }
}

std::vector<glm::vec2> clipConvexPolygons(
    const std::vector<glm::vec2>& subject,
    const std::vector<glm::vec2>& clip
) {
    if (subject.size() < 3u || clip.size() < 3u) {
        return {};
    }
    std::vector<glm::vec2> clipCCW = clip;
    ensureCCW(clipCCW);

    std::vector<glm::vec2> output = subject;
    for (std::size_t i = 0; i < clipCCW.size() && output.size() >= 3u; ++i) {
        const std::vector<glm::vec2> input = output;
        output.clear();
        const glm::vec2& edgeStart = clipCCW[i];
        const glm::vec2& edgeEnd = clipCCW[(i + 1u) % clipCCW.size()];
        const glm::vec2 edgeDir = edgeEnd - edgeStart;

        for (std::size_t j = 0; j < input.size(); ++j) {
            const glm::vec2& current = input[j];
            const glm::vec2& previous = input[(j + input.size() - 1u) % input.size()];
            const float currentSide = cross2(edgeDir, current - edgeStart);
            const float previousSide = cross2(edgeDir, previous - edgeStart);
            const bool currentInside = currentSide >= -kPolygonEpsilon;
            const bool previousInside = previousSide >= -kPolygonEpsilon;

            if (currentInside) {
                if (!previousInside) {
                    const glm::vec2 d = current - previous;
                    const float denom = cross2(edgeDir, d);
                    if (std::abs(denom) > kPlaneEpsilon) {
                        const float t = std::clamp(
                            cross2(edgeDir, edgeStart - previous) / denom, 0.0f, 1.0f);
                        output.push_back(previous + t * d);
                    }
                }
                output.push_back(current);
            } else if (previousInside) {
                const glm::vec2 d = current - previous;
                const float denom = cross2(edgeDir, d);
                if (std::abs(denom) > kPlaneEpsilon) {
                    const float t = std::clamp(
                        cross2(edgeDir, edgeStart - previous) / denom, 0.0f, 1.0f);
                    output.push_back(previous + t * d);
                }
            }
        }
    }
    return output;
}

std::vector<glm::vec2> clipConvexPolygonAgainstHalfPlane(
    const std::vector<glm::vec2>& polygon,
    const glm::vec2& lineA,
    const glm::vec2& lineB,
    bool keepLeft,
    float tolerance
) {
    if (polygon.size() < 3u) {
        return {};
    }

    const glm::vec2 lineDir = lineB - lineA;
    if (glm::length(lineDir) <= kPolygonEpsilon) {
        return polygon;
    }

    auto signedSide = [&](const glm::vec2& point) {
        const float side = cross2(lineDir, point - lineA);
        return keepLeft ? side : -side;
    };

    std::vector<glm::vec2> output{};
    output.reserve(polygon.size() + 1u);
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const glm::vec2& current = polygon[index];
        const glm::vec2& previous = polygon[(index + polygon.size() - 1u) % polygon.size()];
        const float currentSide = signedSide(current);
        const float previousSide = signedSide(previous);
        const bool currentInside = currentSide >= -tolerance;
        const bool previousInside = previousSide >= -tolerance;

        if (currentInside != previousInside) {
            const glm::vec2 segmentDir = current - previous;
            const float denominator = cross2(lineDir, segmentDir);
            if (std::abs(denominator) > kPlaneEpsilon) {
                const float t = std::clamp(cross2(lineDir, lineA - previous) / denominator, 0.0f, 1.0f);
                output.push_back(previous + t * segmentDir);
            }
        }
        if (currentInside) {
            output.push_back(current);
        }
    }

    return normalizePolygonVertices(output);
}

bool convexFootprintInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const std::vector<glm::vec2>& footprint,
    float elevationY,
    const std::vector<std::size_t>* candidateCells
) {
    const float sweptArea = std::abs(polygonSignedArea(footprint));
    if (sweptArea <= kPolygonEpsilon) {
        return true;
    }

    glm::vec2 boundsMin(std::numeric_limits<float>::max());
    glm::vec2 boundsMax(std::numeric_limits<float>::lowest());
    for (const glm::vec2& point : footprint) {
        boundsMin = glm::min(boundsMin, point);
        boundsMax = glm::max(boundsMax, point);
    }
    // Subtract the walkable cells from the footprint instead of adding clipped
    // areas. The latter double-counts overlaps in imported/authored meshes and
    // can accept a footprint that actually crosses a hole.
    std::vector<std::vector<glm::vec2>> uncovered{footprint};
    const double coverageTolerance = std::max(
        static_cast<double>(kPolygonEpsilon) * 0.25,
        static_cast<double>(sweptArea) * 1.0e-6
    );
    const auto cellCouldOverlapFootprint = [&](std::size_t cellIndex) {
        if (cellIndex >= runtime.bakedCells.size()) {
            return false;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        return std::abs(elevationY - cell.elevationY) <=
                kLayerGroupingEpsilon &&
            (cellIndex >= runtime.bakedCellMinXZ.size() ||
             cellIndex >= runtime.bakedCellMaxXZ.size() ||
             boundsOverlapXZ(
                 boundsMin,
                 boundsMax,
                 runtime.bakedCellMinXZ[cellIndex],
                 runtime.bakedCellMaxXZ[cellIndex]));
    };
    if (!runtime.bakedCellsHaveInteriorOverlap) {
        double coveredArea = 0.0;
        const auto accumulateCell = [&](std::size_t cellIndex) {
            if (!cellCouldOverlapFootprint(cellIndex)) {
                return;
            }
            const std::vector<glm::vec2> clipped = clipConvexPolygons(
                footprint,
                runtime.bakedCells[cellIndex].verticesXZ
            );
            if (clipped.size() >= 3u) {
                coveredArea += std::abs(
                    static_cast<double>(polygonSignedArea(clipped))
                );
            }
        };
        if (candidateCells != nullptr) {
            for (std::size_t cellIndex : *candidateCells) {
                accumulateCell(cellIndex);
            }
        } else {
            for (std::size_t cellIndex = 0u;
                 cellIndex < runtime.bakedCells.size();
                 ++cellIndex) {
                accumulateCell(cellIndex);
            }
        }
        return coveredArea + coverageTolerance >=
            static_cast<double>(sweptArea);
    }
    const auto subtractCell = [&](std::size_t cellIndex) {
        if (uncovered.empty() || !cellCouldOverlapFootprint(cellIndex)) {
            return;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        const glm::vec2 cellMin = cellIndex < runtime.bakedCellMinXZ.size()
            ? runtime.bakedCellMinXZ[cellIndex]
            : polygonBoundsXZ(cell.verticesXZ).first;
        const glm::vec2 cellMax = cellIndex < runtime.bakedCellMaxXZ.size()
            ? runtime.bakedCellMaxXZ[cellIndex]
            : polygonBoundsXZ(cell.verticesXZ).second;
        std::vector<std::vector<glm::vec2>> remaining{};
        for (const std::vector<glm::vec2>& piece : uncovered) {
            std::vector<std::vector<glm::vec2>> pieces =
                subtractConvexPolygon(
                    piece,
                    cell.verticesXZ,
                    cellMin,
                    cellMax
                );
            for (std::vector<glm::vec2>& outside : pieces) {
                remaining.push_back(std::move(outside));
            }
        }
        uncovered = std::move(remaining);
    };
    if (candidateCells != nullptr) {
        for (std::size_t cellIndex : *candidateCells) {
            subtractCell(cellIndex);
            if (uncovered.empty()) {
                break;
            }
        }
    } else {
        for (std::size_t cellIndex = 0u;
             cellIndex < runtime.bakedCells.size() && !uncovered.empty();
             ++cellIndex) {
            subtractCell(cellIndex);
        }
    }
    double uncoveredArea = 0.0;
    for (const std::vector<glm::vec2>& piece : uncovered) {
        uncoveredArea += std::abs(
            static_cast<double>(polygonSignedArea(piece))
        );
        if (uncoveredArea > coverageTolerance) {
            return false;
        }
    }
    return true;
}


}  // namespace core::navigation_detail
