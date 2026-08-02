#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>

namespace core::navigation_detail {

std::string canonicalPolygonKey(const std::vector<glm::vec2>& vertices) {
    std::vector<glm::vec2> normalized = normalizePolygonVertices(vertices);
    if (!polygonHasArea(normalized)) {
        return {};
    }

    std::vector<QuantizedVec2> quantized{};
    quantized.reserve(normalized.size());
    for (const glm::vec2& vertex : normalized) {
        quantized.push_back(quantizeVec2(vertex));
    }

    std::size_t bestStart = 0u;
    for (std::size_t candidate = 1u; candidate < quantized.size(); ++candidate) {
        for (std::size_t offset = 0u; offset < quantized.size(); ++offset) {
            const QuantizedVec2& lhs = quantized[(candidate + offset) % quantized.size()];
            const QuantizedVec2& rhs = quantized[(bestStart + offset) % quantized.size()];
            if (lhs == rhs) {
                continue;
            }
            if (lhs < rhs) {
                bestStart = candidate;
            }
            break;
        }
    }

    std::ostringstream key{};
    for (std::size_t offset = 0u; offset < quantized.size(); ++offset) {
        const QuantizedVec2& vertex = quantized[(bestStart + offset) % quantized.size()];
        key << vertex.x << "," << vertex.y << ";";
    }
    return key.str();
}

std::vector<glm::vec2> buildConvexHull(std::vector<glm::vec2> points) {
    if (points.size() < 3u) {
        return {};
    }

    std::sort(points.begin(), points.end(), [](const glm::vec2& lhs, const glm::vec2& rhs) {
        if (lhs.x != rhs.x) {
            return lhs.x < rhs.x;
        }
        return lhs.y < rhs.y;
    });
    points.erase(std::unique(points.begin(), points.end(), [](const glm::vec2& lhs, const glm::vec2& rhs) {
        return nearlyEqualVec2(lhs, rhs);
    }), points.end());
    if (points.size() < 3u) {
        return {};
    }

    std::vector<glm::vec2> lower{};
    for (const glm::vec2& point : points) {
        while (lower.size() >= 2u &&
               triArea2(lower[lower.size() - 2u], lower.back(), point) <= kPolygonEpsilon) {
            lower.pop_back();
        }
        lower.push_back(point);
    }

    std::vector<glm::vec2> upper{};
    for (auto it = points.rbegin(); it != points.rend(); ++it) {
        while (upper.size() >= 2u &&
               triArea2(upper[upper.size() - 2u], upper.back(), *it) <= kPolygonEpsilon) {
            upper.pop_back();
        }
        upper.push_back(*it);
    }

    lower.pop_back();
    upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return normalizePolygonVertices(lower);
}

std::vector<SharedPortalResult> sharedBoundaryPortals(
    const NavRuntimeCell& lhs,
    const NavRuntimeCell& rhs
) {
    std::vector<SharedPortalResult> portals{};
    for (std::size_t lhsIndex = 0; lhsIndex < lhs.verticesXZ.size(); ++lhsIndex) {
        const glm::vec2& lhsA = lhs.verticesXZ[lhsIndex];
        const glm::vec2& lhsB = lhs.verticesXZ[(lhsIndex + 1u) % lhs.verticesXZ.size()];
        const glm::vec2 lhsEdge = lhsB - lhsA;
        const float lhsLength = glm::length(lhsEdge);
        if (lhsLength <= kPolygonEpsilon) {
            continue;
        }
        const glm::vec2 axis = lhsEdge / lhsLength;

        for (std::size_t rhsIndex = 0; rhsIndex < rhs.verticesXZ.size(); ++rhsIndex) {
            const glm::vec2& rhsA = rhs.verticesXZ[rhsIndex];
            const glm::vec2& rhsB = rhs.verticesXZ[(rhsIndex + 1u) % rhs.verticesXZ.size()];
            const glm::vec2 rhsEdge = rhsB - rhsA;
            const float rhsLength = glm::length(rhsEdge);
            if (rhsLength <= kPolygonEpsilon ||
                std::abs(cross2(axis, rhsEdge / rhsLength)) > kPolygonEpsilon ||
                std::abs(cross2(axis, rhsA - lhsA)) > kPolygonEpsilon) {
                continue;
            }

            const float rhsStart = glm::dot(rhsA - lhsA, axis);
            const float rhsEnd = glm::dot(rhsB - lhsA, axis);
            const float overlapStart = std::max(0.0f, std::min(rhsStart, rhsEnd));
            const float overlapEnd = std::min(lhsLength, std::max(rhsStart, rhsEnd));
            // Sub-millimetric overlaps are numerical slivers, not useful
            // traversable portals. Keeping them can make several faces appear
            // incident to the same edge after the conforming Polyanya split.
            if (overlapEnd - overlapStart <= kPortalBroadPhaseEpsilon) {
                continue;
            }
            portals.push_back(SharedPortalResult{
                lhsA + axis * overlapStart,
                lhsA + axis * overlapEnd
            });
        }
    }
    return portals;
}

std::optional<SharedPortalResult> sharedBoundaryPortal(
    const NavRuntimeCell& lhs,
    const NavRuntimeCell& rhs
) {
    std::vector<SharedPortalResult> portals =
        sharedBoundaryPortals(lhs, rhs);
    if (portals.empty()) {
        return std::nullopt;
    }
    return portals.front();
}

std::optional<std::vector<glm::vec2>> tryMergeConvexCells(
    const NavRuntimeCell& lhs,
    const NavRuntimeCell& rhs
) {
    if (std::abs(lhs.elevationY - rhs.elevationY) > kLayerGroupingEpsilon) {
        return std::nullopt;
    }
    if (!sharedBoundaryPortal(lhs, rhs).has_value()) {
        return std::nullopt;
    }

    std::vector<glm::vec2> points = lhs.verticesXZ;
    for (const glm::vec2& vertex : rhs.verticesXZ) {
        if (std::find_if(points.begin(), points.end(), [&](const glm::vec2& existing) {
                return nearlyEqualVec2(existing, vertex);
            }) == points.end()) {
            points.push_back(vertex);
        }
    }

    std::vector<glm::vec2> hull = buildConvexHull(points);
    if (!polygonHasArea(hull)) {
        return std::nullopt;
    }

    // The monotone hull is assembled from float geometry. With very long
    // world-boundary edges, float cross products can retain a point that is
    // microscopically inside the true double-precision hull. Polyanya performs
    // its predicates in double, so remove those non-left turns before a merged
    // cell becomes part of the runtime mesh.
    bool removedImpreciseTurn = true;
    while (removedImpreciseTurn && hull.size() > 3u) {
        removedImpreciseTurn = false;
        for (std::size_t index = 0u; index < hull.size(); ++index) {
            if (preciseTriArea2(
                    hull[(index + hull.size() - 1u) % hull.size()],
                    hull[index],
                    hull[(index + 1u) % hull.size()]) <=
                static_cast<double>(kPolygonEpsilon)) {
                hull.erase(
                    hull.begin() + static_cast<std::ptrdiff_t>(index));
                removedImpreciseTurn = true;
                break;
            }
        }
    }

    const float mergedArea = std::abs(polygonSignedArea(hull));
    const float inputArea = std::abs(polygonSignedArea(lhs.verticesXZ)) + std::abs(polygonSignedArea(rhs.verticesXZ));
    if (std::abs(mergedArea - inputArea) > 0.001f) {
        return std::nullopt;
    }
    return hull;
}

void mergeAdjacentConvexCellsInternal(
    std::vector<NavRuntimeCell>& cells,
    std::vector<std::vector<std::size_t>>* cellToPolygonIndices
) {
    if (cells.size() < 2u) {
        return;
    }
    if (cellToPolygonIndices != nullptr && cellToPolygonIndices->size() != cells.size()) {
        return;
    }

    struct CellBounds {
        glm::vec2 minXZ{0.0f};
        glm::vec2 maxXZ{0.0f};
    };
    struct MergeCandidate {
        std::size_t lhs{0u};
        std::size_t rhs{0u};
        std::uint64_t lhsVersion{0u};
        std::uint64_t rhsVersion{0u};
        float portalLength{0.0f};
        std::vector<glm::vec2> mergedVertices{};

        bool operator<(const MergeCandidate& other) const {
            if (mergedVertices.size() != other.mergedVertices.size()) {
                return mergedVertices.size() > other.mergedVertices.size();
            }
            if (portalLength != other.portalLength) {
                return portalLength < other.portalLength;
            }
            if (lhs != other.lhs) {
                return lhs > other.lhs;
            }
            return rhs > other.rhs;
        }
    };
    struct MergeCandidatePriority {
        bool preserveScanOrder{false};

        bool operator()(const MergeCandidate& lhs, const MergeCandidate& rhs) const {
            if (!preserveScanOrder) {
                return lhs < rhs;
            }
            if (lhs.lhs != rhs.lhs) {
                return lhs.lhs > rhs.lhs;
            }
            return lhs.rhs > rhs.rhs;
        }
    };

    std::vector<std::uint8_t> active(cells.size(), 1u);
    std::vector<std::uint64_t> versions(cells.size(), 0u);
    std::vector<CellBounds> bounds{};
    bounds.reserve(cells.size());
    for (const NavRuntimeCell& cell : cells) {
        const auto [minXZ, maxXZ] = polygonBoundsXZ(cell.verticesXZ);
        bounds.push_back(CellBounds{minXZ, maxXZ});
    }

    std::priority_queue<
        MergeCandidate,
        std::vector<MergeCandidate>,
        MergeCandidatePriority
    > candidates(MergeCandidatePriority{cellToPolygonIndices != nullptr});
    const auto enqueueCandidate = [&](std::size_t lhsIndex, std::size_t rhsIndex) {
        if (lhsIndex == rhsIndex || active[lhsIndex] == 0u || active[rhsIndex] == 0u) {
            return;
        }
        if (lhsIndex > rhsIndex) {
            std::swap(lhsIndex, rhsIndex);
        }
        if (cellToPolygonIndices != nullptr &&
            (*cellToPolygonIndices)[lhsIndex] != (*cellToPolygonIndices)[rhsIndex]) {
            return;
        }
        if (!boundsOverlapXZ(
                bounds[lhsIndex].minXZ,
                bounds[lhsIndex].maxXZ,
                bounds[rhsIndex].minXZ,
                bounds[rhsIndex].maxXZ)) {
            return;
        }
        const auto portal = sharedBoundaryPortal(cells[lhsIndex], cells[rhsIndex]);
        if (!portal.has_value()) {
            return;
        }
        auto merged = tryMergeConvexCells(cells[lhsIndex], cells[rhsIndex]);
        if (!merged.has_value()) {
            return;
        }
        candidates.push(MergeCandidate{
            lhsIndex,
            rhsIndex,
            versions[lhsIndex],
            versions[rhsIndex],
            glm::length(portal->b - portal->a),
            std::move(*merged)
        });
    };

    std::vector<std::size_t> sweepOrder(cells.size());
    for (std::size_t index = 0; index < cells.size(); ++index) {
        sweepOrder[index] = index;
    }
    std::sort(sweepOrder.begin(), sweepOrder.end(), [&](std::size_t lhs, std::size_t rhs) {
        if (bounds[lhs].minXZ.x != bounds[rhs].minXZ.x) {
            return bounds[lhs].minXZ.x < bounds[rhs].minXZ.x;
        }
        return lhs < rhs;
    });
    for (std::size_t orderIndex = 0; orderIndex < sweepOrder.size(); ++orderIndex) {
        const std::size_t lhsIndex = sweepOrder[orderIndex];
        for (std::size_t candidateOrder = orderIndex + 1u; candidateOrder < sweepOrder.size(); ++candidateOrder) {
            const std::size_t rhsIndex = sweepOrder[candidateOrder];
            if (bounds[rhsIndex].minXZ.x > bounds[lhsIndex].maxXZ.x + kPolygonEpsilon) {
                break;
            }
            enqueueCandidate(lhsIndex, rhsIndex);
        }
    }

    while (!candidates.empty()) {
        MergeCandidate candidate = candidates.top();
        candidates.pop();
        if (active[candidate.lhs] == 0u || active[candidate.rhs] == 0u ||
            versions[candidate.lhs] != candidate.lhsVersion ||
            versions[candidate.rhs] != candidate.rhsVersion) {
            continue;
        }

        const auto [mergedMinXZ, mergedMaxXZ] =
            polygonBoundsXZ(candidate.mergedVertices);
        bool overlapsThirdCell = false;
        for (std::size_t otherIndex = 0u;
             otherIndex < cells.size();
             ++otherIndex) {
            if (otherIndex == candidate.lhs ||
                otherIndex == candidate.rhs ||
                active[otherIndex] == 0u ||
                !boundsOverlapXZ(
                    mergedMinXZ,
                    mergedMaxXZ,
                    bounds[otherIndex].minXZ,
                    bounds[otherIndex].maxXZ)) {
                continue;
            }
            if (convexPolygonsHaveInteriorOverlap(
                    candidate.mergedVertices,
                    cells[otherIndex].verticesXZ)) {
                overlapsThirdCell = true;
                break;
            }
        }
        if (overlapsThirdCell) {
            continue;
        }

        cells[candidate.lhs].verticesXZ = std::move(candidate.mergedVertices);
        ++versions[candidate.lhs];
        active[candidate.rhs] = 0u;
        ++versions[candidate.rhs];
        const auto [minXZ, maxXZ] = polygonBoundsXZ(cells[candidate.lhs].verticesXZ);
        bounds[candidate.lhs] = CellBounds{minXZ, maxXZ};

        for (std::size_t otherIndex = 0; otherIndex < cells.size(); ++otherIndex) {
            enqueueCandidate(candidate.lhs, otherIndex);
        }
    }

    std::vector<NavRuntimeCell> compacted{};
    compacted.reserve(cells.size());
    std::vector<std::vector<std::size_t>> compactedMemberships{};
    if (cellToPolygonIndices != nullptr) {
        compactedMemberships.reserve(cellToPolygonIndices->size());
    }
    for (std::size_t index = 0; index < cells.size(); ++index) {
        if (active[index] != 0u) {
            compacted.push_back(std::move(cells[index]));
            if (cellToPolygonIndices != nullptr) {
                compactedMemberships.push_back(std::move((*cellToPolygonIndices)[index]));
            }
        }
    }
    cells = std::move(compacted);
    if (cellToPolygonIndices != nullptr) {
        *cellToPolygonIndices = std::move(compactedMemberships);
    }
}

void mergeAdjacentConvexCells(std::vector<NavRuntimeCell>& cells) {
    mergeAdjacentConvexCellsInternal(cells, nullptr);
}


}  // namespace core::navigation_detail
