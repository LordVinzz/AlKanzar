#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailCorridor.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailTraversal.hpp"

#include <algorithm>
#include <limits>
#include <queue>

namespace core::navigation_detail {

std::optional<SharedPortalResult> safePortalForTraversal(
    const NavigationSolveView& runtime,
    std::size_t fromCellIndex,
    const NavGraphEdge& edge,
    const AgentClearanceProfile& profile
) {
    if (edge.viaLink || edge.targetCellIndex >= runtime.bakedCells.size()) {
        return std::nullopt;
    }
    const SharedPortalResult rawPortal{edge.portalA, edge.portalB};
    if (profile.empty() ||
        portalIsInternalToSharedAuthoredPolygon(
            runtime,
            fromCellIndex,
            edge.targetCellIndex,
            rawPortal
        )) {
        return rawPortal;
    }
    if (profile.shape == AgentClearanceShape::Box ||
        profile.shape == AgentClearanceShape::Sphere) {
        const glm::vec2 delta = edge.portalB - edge.portalA;
        const float length = glm::length(delta);
        // The heading at a graph portal is not known until an actual segment
        // is tested. Use a superset of every heading's feasible interval here;
        // exact swept-box validation below rejects unsafe arcs. This keeps an
        // anisotropic or off-centre box from losing its only valid heading.
        const float minimumExtent =
            profile.shape == AgentClearanceShape::Sphere
            ? profile.sphereRadius
            : std::min(
                profile.boxHalfExtentsXZ.x,
                profile.boxHalfExtentsXZ.y
            );
        const float minimumInset = std::max(
            0.0f,
            minimumExtent - glm::length(profile.centerXZ)
        );
        if (length <= minimumInset * 2.0f + kPolygonEpsilon) {
            return std::nullopt;
        }
        const glm::vec2 direction = delta / length;
        return SharedPortalResult{
            edge.portalA + direction * minimumInset,
            edge.portalB - direction * minimumInset,
        };
    }
    return shrinkPortal(
        edge.portalA,
        edge.portalB,
        profile,
        runtime.bakedCellCenters[edge.targetCellIndex] -
            runtime.bakedCellCenters[fromCellIndex]
    );
}

std::optional<std::vector<NavCorridorStep>> findAStarCorridor(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled,
    PortalSamplingMode samplingMode,
    const std::vector<std::pair<std::size_t, std::size_t>>&
        blockedTraversals
) {
    if (runtime.bakedCells.empty() || runtime.graph.size() != runtime.bakedCells.size()) {
        return std::nullopt;
    }

    // A state is an arrival sample on one directed portal (or link).  Keeping
    // arrivals distinct avoids the classic centroid-A* bug where reaching a
    // large polygon through the wrong side hides a shorter corridor.  Portal
    // endpoints are essential: shortest paths bend at boundary vertices, while
    // using only a portal midpoint can rank the wrong corridor before funneling.
    std::size_t graphEdgeCount = 0u;
    for (const std::vector<NavGraphEdge>& edges : runtime.graph) {
        graphEdgeCount += edges.size();
    }
    std::vector<NavTraversalState> traversals{};
    traversals.reserve(graphEdgeCount * 5u);
    std::vector<std::vector<std::size_t>> outgoingTraversals(
        runtime.graph.size()
    );
    const glm::vec2 startXZ(
        endpoints.resolvedStart.x,
        endpoints.resolvedStart.z
    );
    const glm::vec2 destinationXZ(
        endpoints.resolvedDestination.x,
        endpoints.resolvedDestination.z
    );
    for (std::size_t fromCellIndex = 0u;
         fromCellIndex < runtime.graph.size();
         ++fromCellIndex) {
        for (std::size_t edgeIndex = 0u;
             edgeIndex < runtime.graph[fromCellIndex].size();
             ++edgeIndex) {
            if (std::find(
                    blockedTraversals.begin(),
                    blockedTraversals.end(),
                    std::pair{fromCellIndex, edgeIndex}
                ) != blockedTraversals.end()) {
                continue;
            }
            const NavGraphEdge& edge = runtime.graph[fromCellIndex][edgeIndex];
            if (edge.targetCellIndex >= runtime.bakedCells.size()) {
                continue;
            }
            if (edge.viaLink) {
                if (!pointInsideAuthoredWalkableSurface(
                        runtime,
                        edge.linkStartPoint) ||
                    !pointInsideAuthoredWalkableSurface(
                        runtime,
                        edge.linkEndPoint)) {
                    continue;
                }
                if (!profile.empty()) {
                    const glm::vec2 linkDirection =
                        travelDirectionForSegment(
                            edge.linkStartPoint,
                            edge.linkEndPoint
                        );
                    if (!pointInsideAuthoredWalkableSurfaceWithClearance(
                            runtime,
                            edge.linkStartPoint,
                            profile,
                            linkDirection) ||
                        !pointInsideAuthoredWalkableSurfaceWithClearance(
                            runtime,
                            edge.linkEndPoint,
                            profile,
                            linkDirection)) {
                        continue;
                    }
                }
                outgoingTraversals[fromCellIndex].push_back(
                    traversals.size()
                );
                traversals.push_back(NavTraversalState{
                    fromCellIndex,
                    edge.targetCellIndex,
                    edgeIndex,
                    edge.linkEndPoint,
                    std::nullopt,
                    true
                });
                continue;
            }
            const std::optional<SharedPortalResult> safePortal =
                safePortalForTraversal(
                    runtime,
                    fromCellIndex,
                    edge,
                    profile
                );
            if (!safePortal.has_value()) {
                continue;
            }

            const glm::vec2 midpoint =
                (safePortal->a + safePortal->b) * 0.5f;
            std::vector<glm::vec2> portalSamples{midpoint};
            if (samplingMode == PortalSamplingMode::Geometry) {
                portalSamples = {
                    safePortal->a,
                    midpoint,
                    safePortal->b,
                    closestPointOnSegmentXZ(
                        startXZ,
                        safePortal->a,
                        safePortal->b
                    ),
                    closestPointOnSegmentXZ(
                        destinationXZ,
                        safePortal->a,
                        safePortal->b
                    ),
                };
            }
            std::vector<glm::vec2> uniqueSamples{};
            uniqueSamples.reserve(portalSamples.size());
            for (const glm::vec2& sample : portalSamples) {
                if (std::none_of(
                        uniqueSamples.begin(),
                        uniqueSamples.end(),
                        [&](const glm::vec2& existing) {
                            return nearlyEqualVec2(existing, sample);
                        })) {
                    uniqueSamples.push_back(sample);
                }
            }
            for (const glm::vec2& sample : uniqueSamples) {
                outgoingTraversals[fromCellIndex].push_back(
                    traversals.size()
                );
                traversals.push_back(NavTraversalState{
                    fromCellIndex,
                    edge.targetCellIndex,
                    edgeIndex,
                    glm::vec3(
                        sample.x,
                        runtime.bakedCells[
                            edge.targetCellIndex
                        ].elevationY,
                        sample.y
                    ),
                    safePortal,
                    true
                });
            }
        }
    }
    const std::size_t traversalCount = traversals.size();
    const std::size_t stateCount =
        traversalCount + runtime.bakedCells.size();
    const std::size_t invalidState =
        std::numeric_limits<std::size_t>::max();

    std::vector<std::uint8_t> targetCells(runtime.bakedCells.size(), 0u);
    for (std::size_t targetCellIndex : endpoints.targetCells) {
        if (targetCellIndex < targetCells.size()) {
            targetCells[targetCellIndex] = 1u;
        }
    }

    struct QueueItem {
        float fScore{0.0f};
        float gScore{0.0f};
        std::size_t stateIndex{0u};

        bool operator<(const QueueItem& other) const {
            if (fScore != other.fScore) {
                return fScore > other.fScore;
            }
            return gScore < other.gScore;
        }
    };

    const auto stateCell = [&](std::size_t stateIndex) {
        return stateIndex < traversalCount
            ? traversals[stateIndex].toCellIndex
            : stateIndex - traversalCount;
    };
    const auto stateAnchor = [&](std::size_t stateIndex) {
        return stateIndex < traversalCount
            ? traversals[stateIndex].anchor
            : endpoints.resolvedStart;
    };

    std::vector<float> gScores(stateCount, std::numeric_limits<float>::max());
    std::vector<std::size_t> parents(stateCount, invalidState);
    std::priority_queue<QueueItem> open{};
    for (std::size_t startCellIndex : endpoints.startCells) {
        if (startCellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const std::size_t startState = traversalCount + startCellIndex;
        gScores[startState] = 0.0f;
        open.push(QueueItem{
            glm::distance(endpoints.resolvedStart, endpoints.resolvedDestination),
            0.0f,
            startState
        });
    }
    if (open.empty()) {
        return std::nullopt;
    }

    float bestGoalCost = std::numeric_limits<float>::max();
    std::size_t bestGoalState = invalidState;
    std::size_t expansionCount = 0u;
    while (!open.empty()) {
        if ((expansionCount++ & 31u) == 0u &&
            cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed)) {
            return std::nullopt;
        }

        const QueueItem current = open.top();
        open.pop();
        if (current.fScore >= bestGoalCost - kPolygonEpsilon) {
            break;
        }
        if (current.stateIndex >= stateCount ||
            current.gScore > gScores[current.stateIndex] + kPolygonEpsilon) {
            continue;
        }

        const std::size_t currentCellIndex = stateCell(current.stateIndex);
        if (currentCellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const glm::vec3 currentAnchor = stateAnchor(current.stateIndex);
        if (targetCells[currentCellIndex] != 0u) {
            const float goalCost =
                current.gScore +
                glm::distance(currentAnchor, endpoints.resolvedDestination);
            if (goalCost < bestGoalCost) {
                bestGoalCost = goalCost;
                bestGoalState = current.stateIndex;
            }
        }

        for (std::size_t traversalIndex :
             outgoingTraversals[currentCellIndex]) {
            const NavTraversalState& traversal = traversals[traversalIndex];
            if (!traversal.traversable) {
                continue;
            }
            const NavGraphEdge& edge =
                runtime.graph[currentCellIndex][traversal.edgeIndex];
            if (edge.viaLink &&
                !segmentInsideAuthoredWalkableSurfaceWithClearance(
                    runtime,
                    currentAnchor,
                    edge.linkStartPoint,
                    profile)) {
                continue;
            }
            const float stepCost = edge.viaLink
                ? glm::distance(currentAnchor, edge.linkStartPoint) +
                    glm::distance(edge.linkStartPoint, edge.linkEndPoint)
                : glm::distance(currentAnchor, traversal.anchor);
            const float candidateG = current.gScore + stepCost;
            if (candidateG >= gScores[traversalIndex] - kPolygonEpsilon) {
                continue;
            }
            gScores[traversalIndex] = candidateG;
            parents[traversalIndex] = current.stateIndex;
            open.push(QueueItem{
                candidateG +
                    glm::distance(traversal.anchor, endpoints.resolvedDestination),
                candidateG,
                traversalIndex
            });
        }
    }
    if (bestGoalState == invalidState) {
        return std::nullopt;
    }

    std::vector<NavCorridorStep> reversed{};
    for (std::size_t stateIndex = bestGoalState;
         stateIndex < traversalCount;
         stateIndex = parents[stateIndex]) {
        const NavTraversalState& traversal = traversals[stateIndex];
        reversed.push_back(NavCorridorStep{
            traversal.fromCellIndex,
            traversal.toCellIndex,
            traversal.edgeIndex,
            traversal.safePortal
        });
        if (parents[stateIndex] == invalidState) {
            return std::nullopt;
        }
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}


}  // namespace core::navigation_detail
