#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailCorridor.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPath.hpp"
#include "core/navigation/NavigationDetailRuntime.hpp"
#include "core/navigation/NavigationDetailTraversal.hpp"
#include "core/navigation/NavigationDetailVisibility.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace core::navigation_detail {

std::optional<std::vector<glm::vec3>> solveCorridorClearancePath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled,
    bool allowVisibilityFallback,
    bool restrictToCorridor,
    VisibilityTraversalCache* sharedVisibilityCache
) {
    const auto isCancelled = [&]() {
        return cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed);
    };
    if (profile.empty() || isCancelled() || !allowVisibilityFallback) {
        return std::nullopt;
    }

    constexpr std::size_t kMaximumLocalVisibilityNodes = 72u;
    constexpr std::size_t kMaximumRefinedVisibilityNodes = 96u;
    constexpr float kMaximumUnrefinedStretch = 1.5f;
    std::optional<VisibilityNodeSet> nodeSet = buildVisibilityNodes(
        runtime,
        endpoints,
        corridor,
        profile,
        cancelled,
        restrictToCorridor
    );
    if (!nodeSet.has_value()) {
        return std::nullopt;
    }
    std::vector<glm::vec3>& nodes = nodeSet->nodes;

    struct QueueItem {
        float fScore{0.0f};
        float gScore{0.0f};
        std::size_t nodeIndex{0u};

        bool operator<(const QueueItem& other) const {
            return fScore > other.fScore;
        }
    };
    std::unordered_map<std::uint64_t, bool> visibilityCache{};
    visibilityCache.reserve(kMaximumRefinedVisibilityNodes * 32u);
    const auto canTraverse = [&](std::size_t lhs, std::size_t rhs) {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(lhs) << 32u) |
            static_cast<std::uint64_t>(rhs);
        if (const auto found = visibilityCache.find(key);
            found != visibilityCache.end()) {
            return found->second;
        }
        const VisibilitySegmentKey sharedKey{
            exactLayerPoint(nodes[lhs]),
            exactLayerPoint(nodes[rhs]),
        };
        // Every corridor in this query uses the same immutable runtime and
        // clearance profile. Keep the key directed (box offsets can make a
        // sweep direction-sensitive) and use exact float bits so caching can
        // never merge two merely-near tangent segments.
        if (sharedVisibilityCache != nullptr) {
            if (const auto shared = sharedVisibilityCache->find(sharedKey);
                shared != sharedVisibilityCache->end()) {
                visibilityCache.emplace(key, shared->second);
                return shared->second;
            }
        }
        const bool visible =
            segmentInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                nodes[lhs],
                nodes[rhs],
                profile
            );
        if (sharedVisibilityCache != nullptr) {
            sharedVisibilityCache->emplace(sharedKey, visible);
        }
        visibilityCache.emplace(key, visible);
        return visible;
    };

    struct VisibilitySearchResult {
        float length{std::numeric_limits<float>::max()};
        std::vector<std::size_t> path{};
    };
    const auto searchVisibilityGraph = [&]()
        -> std::optional<VisibilitySearchResult> {
        const std::size_t nodeCount = nodes.size();
        std::vector<float> distances(
            nodeCount,
            std::numeric_limits<float>::max());
        std::vector<std::size_t> parents(
            nodeCount,
            std::numeric_limits<std::size_t>::max());
        std::vector<std::uint8_t> closed(nodeCount, 0u);
        std::priority_queue<QueueItem> open{};
        distances[0] = 0.0f;
        open.push(QueueItem{
            glm::distance(nodes[0], nodes[1]),
            0.0f,
            0u,
        });
        std::optional<VisibilitySearchResult> best{};
        std::size_t expansionCount = 0u;
        while (!open.empty()) {
            if ((expansionCount++ & 15u) == 0u && isCancelled()) {
                return std::nullopt;
            }
            const QueueItem current = open.top();
            open.pop();
            if (best.has_value() &&
                current.fScore >= best->length - kPolygonEpsilon) {
                break;
            }
            if (current.gScore >
                    distances[current.nodeIndex] + kPolygonEpsilon ||
                closed[current.nodeIndex] != 0u) {
                continue;
            }
            closed[current.nodeIndex] = 1u;

            const auto considerNode = [&](std::size_t nextNode) {
                if (nextNode == 0u || nextNode == current.nodeIndex ||
                    nextNode >= nodeCount || closed[nextNode] != 0u) {
                    return;
                }
                const float candidate =
                    distances[current.nodeIndex] +
                    glm::distance(nodes[current.nodeIndex], nodes[nextNode]);
                const float estimate = candidate +
                    (nextNode == 1u
                        ? 0.0f
                        : glm::distance(nodes[nextNode], nodes[1]));
                if ((best.has_value() &&
                     estimate >= best->length - kPolygonEpsilon) ||
                    (nextNode != 1u &&
                     candidate >= distances[nextNode] - kPolygonEpsilon) ||
                    !canTraverse(current.nodeIndex, nextNode)) {
                    return;
                }
                if (nextNode == 1u) {
                    VisibilitySearchResult result{};
                    result.length = candidate;
                    result.path.push_back(1u);
                    for (std::size_t pathNode = current.nodeIndex;;) {
                        result.path.push_back(pathNode);
                        if (pathNode == 0u) {
                            break;
                        }
                        pathNode = parents[pathNode];
                        if (pathNode ==
                            std::numeric_limits<std::size_t>::max()) {
                            return;
                        }
                    }
                    std::reverse(result.path.begin(), result.path.end());
                    best = std::move(result);
                    return;
                }
                distances[nextNode] = candidate;
                parents[nextNode] = current.nodeIndex;
                open.push(QueueItem{estimate, candidate, nextNode});
            };

            for (std::size_t nextNode = 1u;
                 nextNode < nodeCount;
                 ++nextNode) {
                considerNode(nextNode);
            }
        }
        return best;
    };

    std::optional<VisibilitySearchResult> visibilityResult =
        searchVisibilityGraph();
    if (!visibilityResult.has_value() &&
        nodes.size() < kMaximumLocalVisibilityNodes &&
        nodeSet->nextRankedNodeIndex < nodeSet->rankedNodes.size()) {
        if (!appendRankedVisibilityNodes(
                *nodeSet,
                kMaximumLocalVisibilityNodes,
                runtime,
                profile,
                cancelled)) {
            return std::nullopt;
        }
        visibilityResult = searchVisibilityGraph();
    }
    if (profile.shape == AgentClearanceShape::Box &&
        restrictToCorridor &&
        !corridor.empty()) {
        // The cheap radial corner samples normally suffice. A very long
        // result relative to the raw funnel is a strong signal that a
        // heading-aligned box tangent was missed (notably on long portals,
        // whose midpoint can be hundreds of units away). In that case only,
        // add analytic footprint tangents at the constraining funnel corners.
        std::vector<NavCorridorStep> rawPortalCorridor = corridor;
        for (NavCorridorStep& step : rawPortalCorridor) {
            if (step.fromCellIndex >= runtime.graph.size() ||
                step.edgeIndex >= runtime.graph[step.fromCellIndex].size()) {
                continue;
            }
            const NavGraphEdge& edge =
                runtime.graph[step.fromCellIndex][step.edgeIndex];
            if (!edge.viaLink) {
                step.safePortal = SharedPortalResult{
                    edge.portalA,
                    edge.portalB,
                };
            }
        }
        const std::optional<std::vector<glm::vec3>> rawGuide =
            buildFunnelPath(runtime, endpoints, rawPortalCorridor);
        const float rawGuideLength = rawGuide.has_value()
            ? pathLength(endpoints.resolvedStart, *rawGuide)
            : std::numeric_limits<float>::max();
        const bool needsTangentRefinement =
            rawGuide.has_value() &&
            (!visibilityResult.has_value() ||
             visibilityResult->length >
                rawGuideLength * kMaximumUnrefinedStretch);
        if (needsTangentRefinement) {
            std::vector<glm::vec3> tangentCandidates{};
            tangentCandidates.reserve(rawGuide->size() * 8u);
            const auto addTangentsForReference = [&](
                const glm::vec3& vertex,
                const glm::vec3& reference,
                bool movingTowardReference
            ) {
                const glm::vec2 vertexXZ(vertex.x, vertex.z);
                const glm::vec2 referenceXZ(reference.x, reference.z);
                for (float lateralSign : {-1.0f, 1.0f}) {
                    for (float longitudinalSign : {-1.0f, 1.0f}) {
                        glm::vec2 forward = normalizeOrFallback(
                            movingTowardReference
                                ? referenceXZ - vertexXZ
                                : vertexXZ - referenceXZ
                        );
                        glm::vec2 origin = vertexXZ;
                        for (int refinement = 0;
                             refinement < 3;
                             ++refinement) {
                            const glm::vec2 footprintForward =
                                clearanceOrientationForward(
                                    profile,
                                    forward
                                );
                            const glm::vec2 right(
                                footprintForward.y,
                                -footprintForward.x
                            );
                            const glm::vec2 center =
                                rotateLocalXZToPlanar(
                                    profile.centerXZ,
                                    footprintForward
                                );
                            const glm::vec2 footprintCorner = center +
                                right * profile.boxHalfExtentsXZ.x *
                                    lateralSign +
                                footprintForward *
                                    profile.boxHalfExtentsXZ.y *
                                    longitudinalSign;
                            origin = vertexXZ - footprintCorner;
                            forward = normalizeOrFallback(
                                movingTowardReference
                                    ? referenceXZ - origin
                                    : origin - referenceXZ,
                                forward
                            );
                        }
                        tangentCandidates.emplace_back(
                            origin.x,
                            vertex.y,
                            origin.y
                        );
                    }
                }
            };
            for (const glm::vec3& guideCorner : *rawGuide) {
                if (nearlyEqualVec3(
                        guideCorner,
                        endpoints.resolvedDestination,
                        kPolygonEpsilon)) {
                    continue;
                }
                addTangentsForReference(
                    guideCorner,
                    endpoints.resolvedStart,
                    false
                );
                addTangentsForReference(
                    guideCorner,
                    endpoints.resolvedDestination,
                    true
                );
            }
            std::stable_sort(
                tangentCandidates.begin(),
                tangentCandidates.end(),
                [&](const glm::vec3& lhs, const glm::vec3& rhs) {
                    const float lhsScore =
                        glm::distance(endpoints.resolvedStart, lhs) +
                        glm::distance(lhs, endpoints.resolvedDestination);
                    const float rhsScore =
                        glm::distance(endpoints.resolvedStart, rhs) +
                        glm::distance(rhs, endpoints.resolvedDestination);
                    return lhsScore < rhsScore;
                }
            );
            std::unordered_set<
                QuantizedLayerPoint,
                QuantizedLayerPointHash
            > selectedNodes{};
            selectedNodes.reserve(nodes.size() + tangentCandidates.size());
            for (const glm::vec3& node : nodes) {
                selectedNodes.insert(quantizeLayerPoint(
                    glm::vec2(node.x, node.z),
                    node.y
                ));
            }
            for (const glm::vec3& candidate : tangentCandidates) {
                if (nodes.size() >= kMaximumRefinedVisibilityNodes ||
                    isCancelled()) {
                    break;
                }
                const QuantizedLayerPoint key = quantizeLayerPoint(
                    glm::vec2(candidate.x, candidate.z),
                    candidate.y
                );
                if (selectedNodes.insert(key).second &&
                    visibilityNodeCanParticipate(runtime, profile, candidate)) {
                    nodes.push_back(candidate);
                }
            }
            if (isCancelled()) {
                return std::nullopt;
            }
            const std::optional<VisibilitySearchResult> refinedResult =
                searchVisibilityGraph();
            if (refinedResult.has_value() &&
                (!visibilityResult.has_value() ||
                 refinedResult->length < visibilityResult->length)) {
                visibilityResult = refinedResult;
            }
        }
    }
    if (!visibilityResult.has_value() || visibilityResult->path.size() < 2u) {
        return std::nullopt;
    }
    std::vector<glm::vec3> result{};
    result.reserve(visibilityResult->path.size() - 1u);
    for (std::size_t pathIndex = 1u;
         pathIndex < visibilityResult->path.size();
         ++pathIndex) {
        result.push_back(nodes[visibilityResult->path[pathIndex]]);
    }
    return result;
}

bool segmentMatchesRuntimeLink(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    float epsilon
) {
    for (const std::vector<NavGraphEdge>& edges : runtime.graph) {
        for (const NavGraphEdge& edge : edges) {
            if (!edge.viaLink) {
                continue;
            }
            if (nearlyEqualVec3(from, edge.linkStartPoint, epsilon) &&
                nearlyEqualVec3(to, edge.linkEndPoint, epsilon)) {
                return true;
            }
        }
    }
    return false;
}


}  // namespace core::navigation_detail
