#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailCorridor.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPath.hpp"
#include "core/navigation/NavigationDetailRuntime.hpp"
#include "core/navigation/NavigationDetailTraversal.hpp"
#include "core/navigation/Polyanya.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <optional>

#include <glm/geometric.hpp>

namespace core::navigation_detail {

std::optional<SolvedPath> solvePathCorners(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const glm::vec3& destination,
    float arrivalRadius,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled
) {
    const auto isCancelled = [&]() {
        return cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed);
    };
    const std::optional<ResolvedPathEndpoints> endpoints =
        resolvePathEndpoints(
            runtime,
            start,
            destination,
            profile,
            cancelled
        );
    if (!endpoints.has_value() || isCancelled()) {
        return std::nullopt;
    }

    if (canSolveDirectPath(runtime, *endpoints, profile)) {
        std::vector<glm::vec3> directCorners{};
        appendPathCorner(directCorners, endpoints->resolvedDestination, arrivalRadius);
        return SolvedPath{
            endpoints->resolvedDestination,
            std::move(directCorners),
            endpoints->resolvedStart
        };
    }

    // Polyanya searches continuous intervals instead of a handful of sampled
    // points on each portal. On a conforming convex mesh it returns the global
    // Euclidean shortest path directly; applying a funnel afterwards would be
    // redundant. Off-mesh links and collider clearance remain on the legacy
    // validated pipeline until they have an exact configuration-space mesh.
    std::optional<std::vector<glm::vec3>> exactPlanarCorners{};
    if (profile.empty() && runtime.polyanyaMesh != nullptr) {
        if (std::optional<navigation_detail::PolyanyaPath> exactPath =
                navigation_detail::findPolyanyaPath(
                    *runtime.polyanyaMesh,
                    endpoints->resolvedStart,
                    endpoints->resolvedDestination,
                    endpoints->startCells,
                    endpoints->targetCells,
                    cancelled);
            exactPath.has_value() &&
            pathSegmentsAreValid(
                runtime,
                endpoints->resolvedStart,
                exactPath->corners,
                profile)) {
            if (exactPath->corners.size() == 1u &&
                glm::distance(
                    endpoints->resolvedStart,
                    exactPath->corners.front()) <= arrivalRadius) {
                exactPath->corners.clear();
            }

            if (runtime.asset.links.empty()) {
                return SolvedPath{
                    endpoints->resolvedDestination,
                    std::move(exactPath->corners),
                    endpoints->resolvedStart
                };
            }

            // A NavLink only matters when it lies on some directed graph path
            // from this query's start set to its target set.  Unresolved links,
            // links in another component and dead-end links must not demote an
            // otherwise exact Polyanya query to the sampled fallback.
            std::vector<std::uint8_t> reachableFromStart(
                runtime.graph.size(),
                0u);
            std::vector<std::size_t> pending{};
            for (std::size_t cell : endpoints->startCells) {
                if (cell < reachableFromStart.size() &&
                    reachableFromStart[cell] == 0u) {
                    reachableFromStart[cell] = 1u;
                    pending.push_back(cell);
                }
            }
            while (!pending.empty()) {
                const std::size_t cell = pending.back();
                pending.pop_back();
                for (const NavGraphEdge& edge : runtime.graph[cell]) {
                    if (edge.targetCellIndex < reachableFromStart.size() &&
                        reachableFromStart[edge.targetCellIndex] == 0u) {
                        reachableFromStart[edge.targetCellIndex] = 1u;
                        pending.push_back(edge.targetCellIndex);
                    }
                }
            }

            std::vector<std::vector<std::size_t>> reverseGraph(
                runtime.graph.size());
            for (std::size_t cell = 0u; cell < runtime.graph.size(); ++cell) {
                for (const NavGraphEdge& edge : runtime.graph[cell]) {
                    if (edge.targetCellIndex < reverseGraph.size()) {
                        reverseGraph[edge.targetCellIndex].push_back(cell);
                    }
                }
            }
            std::vector<std::uint8_t> canReachTarget(runtime.graph.size(), 0u);
            for (std::size_t cell : endpoints->targetCells) {
                if (cell < canReachTarget.size() && canReachTarget[cell] == 0u) {
                    canReachTarget[cell] = 1u;
                    pending.push_back(cell);
                }
            }
            while (!pending.empty()) {
                const std::size_t cell = pending.back();
                pending.pop_back();
                for (std::size_t predecessor : reverseGraph[cell]) {
                    if (canReachTarget[predecessor] == 0u) {
                        canReachTarget[predecessor] = 1u;
                        pending.push_back(predecessor);
                    }
                }
            }

            bool hasRelevantLink = false;
            for (std::size_t cell = 0u;
                 cell < runtime.graph.size() && !hasRelevantLink;
                 ++cell) {
                if (reachableFromStart[cell] == 0u) {
                    continue;
                }
                hasRelevantLink = std::any_of(
                    runtime.graph[cell].begin(),
                    runtime.graph[cell].end(),
                    [&](const NavGraphEdge& edge) {
                        return edge.viaLink &&
                            edge.targetCellIndex < canReachTarget.size() &&
                            canReachTarget[edge.targetCellIndex] != 0u;
                    }
                );
            }
            if (!hasRelevantLink) {
                return SolvedPath{
                    endpoints->resolvedDestination,
                    std::move(exactPath->corners),
                    endpoints->resolvedStart
                };
            }
            // Keep the exact no-link route as a baseline.  The link-aware A*
            // result below is accepted only when its validated final geometry
            // is actually shorter.
            exactPlanarCorners = std::move(exactPath->corners);
        }
    }

    std::vector<std::vector<NavCorridorStep>> corridorCandidates{};
    constexpr std::size_t kMaximumCorridorSearches = 14u;
    std::size_t corridorSearchCount = 0u;
    const auto appendCorridorCandidate = [&](
        PortalSamplingMode samplingMode,
        const std::vector<std::pair<std::size_t, std::size_t>>&
            blockedTraversals,
        const AgentClearanceProfile& corridorProfile
    ) {
        if (corridorSearchCount >= kMaximumCorridorSearches ||
            isCancelled()) {
            return;
        }
        ++corridorSearchCount;
        std::optional<std::vector<NavCorridorStep>> candidate =
            findAStarCorridor(
                runtime,
                *endpoints,
                corridorProfile,
                cancelled,
                samplingMode,
                blockedTraversals
            );
        if (!candidate.has_value()) {
            return;
        }
        const bool duplicate = std::any_of(
            corridorCandidates.begin(),
            corridorCandidates.end(),
            [&](const std::vector<NavCorridorStep>& existing) {
                return existing.size() == candidate->size() &&
                    std::equal(
                        existing.begin(),
                        existing.end(),
                        candidate->begin(),
                        [](const NavCorridorStep& lhs,
                           const NavCorridorStep& rhs) {
                            return lhs.fromCellIndex ==
                                    rhs.fromCellIndex &&
                                lhs.toCellIndex ==
                                    rhs.toCellIndex &&
                                lhs.edgeIndex == rhs.edgeIndex;
                        }
                    );
            }
        );
        if (!duplicate) {
            corridorCandidates.push_back(std::move(*candidate));
        }
    };

    constexpr std::array<PortalSamplingMode, 2u> kSamplingModes{
        PortalSamplingMode::Midpoint,
        PortalSamplingMode::Geometry,
    };
    for (PortalSamplingMode samplingMode : kSamplingModes) {
        appendCorridorCandidate(samplingMode, {}, profile);
    }

    // Bounded k-alternative search: force deviations at evenly distributed
    // portals of the best corridors.  Funnel length, not the A* representative
    // polyline, decides between the resulting corridors below.
    std::vector<std::uint8_t> endpointCells(runtime.graph.size(), 0u);
    for (std::size_t cellIndex : endpoints->startCells) {
        if (cellIndex < endpointCells.size()) {
            endpointCells[cellIndex] = 1u;
        }
    }
    for (std::size_t cellIndex : endpoints->targetCells) {
        if (cellIndex < endpointCells.size()) {
            endpointCells[cellIndex] = 1u;
        }
    }
    bool graphHasBranches = false;
    for (std::size_t cellIndex = 0u;
         cellIndex < runtime.graph.size();
         ++cellIndex) {
        // Interior degree two is just a chain, while degree two at a query
        // endpoint is already a real choice between two homotopies.
        const std::size_t branchDegree =
            endpointCells[cellIndex] != 0u ? 1u : 2u;
        if (runtime.graph[cellIndex].size() > branchDegree) {
            graphHasBranches = true;
            break;
        }
    }
    const bool shouldExploreAlternatives =
        corridorCandidates.size() > 1u || graphHasBranches;
    std::size_t sourceCandidateIndex = 0u;
    while (shouldExploreAlternatives &&
           sourceCandidateIndex < corridorCandidates.size() &&
           corridorSearchCount < kMaximumCorridorSearches) {
        const std::vector<NavCorridorStep> source =
            corridorCandidates[sourceCandidateIndex++];
        if (source.empty()) {
            continue;
        }
        const std::size_t deviationCount =
            std::min<std::size_t>(4u, source.size());
        std::vector<std::size_t> deviationIndices{};
        deviationIndices.reserve(deviationCount);
        for (std::size_t deviation = 0u;
             deviation < deviationCount;
             ++deviation) {
            const std::size_t stepIndex = deviationCount == 1u
                ? 0u
                : deviation * (source.size() - 1u) /
                    (deviationCount - 1u);
            if (deviationIndices.empty() ||
                deviationIndices.back() != stepIndex) {
                deviationIndices.push_back(stepIndex);
            }
        }
        for (std::size_t stepIndex : deviationIndices) {
            const NavCorridorStep& blockedStep = source[stepIndex];
            const std::vector<std::pair<std::size_t, std::size_t>>
                blockedTraversals{
                    {
                        blockedStep.fromCellIndex,
                        blockedStep.edgeIndex,
                    },
                };
            for (PortalSamplingMode samplingMode : kSamplingModes) {
                appendCorridorCandidate(
                    samplingMode,
                    blockedTraversals,
                    profile
                );
            }
            if (corridorSearchCount >= kMaximumCorridorSearches) {
                break;
            }
        }
    }
    if (corridorCandidates.empty() || isCancelled()) {
        if (exactPlanarCorners.has_value() && !isCancelled()) {
            return SolvedPath{
                endpoints->resolvedDestination,
                std::move(*exactPlanarCorners),
                endpoints->resolvedStart
            };
        }
        return std::nullopt;
    }

    struct CorridorPathCandidate {
        const std::vector<NavCorridorStep>* corridor{nullptr};
        std::vector<glm::vec3> rawCorners{};
        float rawLength{std::numeric_limits<float>::max()};
        bool rawIsValid{false};
    };
    std::vector<CorridorPathCandidate> pathCandidates{};
    pathCandidates.reserve(corridorCandidates.size());
    for (std::size_t corridorIndex = 0u;
         corridorIndex < corridorCandidates.size();
         ++corridorIndex) {
        const std::vector<NavCorridorStep>& corridorCandidate =
            corridorCandidates[corridorIndex];
        std::optional<std::vector<glm::vec3>> candidateCorners =
            buildFunnelPath(runtime, *endpoints, corridorCandidate);
        if (!candidateCorners.has_value() || candidateCorners->empty()) {
            continue;
        }
        const float rawLength = pathLength(
            endpoints->resolvedStart,
            *candidateCorners
        );
        pathCandidates.push_back(CorridorPathCandidate{
            &corridorCandidate,
            std::move(*candidateCorners),
            rawLength,
            false,
        });
        // Evaluate after the move; using the stored vector keeps this aggregate
        // construction simple and avoids copying every raw funnel path.
        pathCandidates.back().rawIsValid = pathSegmentsAreValid(
            runtime,
            endpoints->resolvedStart,
            pathCandidates.back().rawCorners,
            profile
        );
    }
    std::sort(
        pathCandidates.begin(),
        pathCandidates.end(),
        [](const CorridorPathCandidate& lhs,
           const CorridorPathCandidate& rhs) {
            return lhs.rawLength < rhs.rawLength;
        }
    );

    std::optional<std::vector<glm::vec3>> corners{};
    float selectedLength = std::numeric_limits<float>::max();
    VisibilityTraversalCache sharedVisibilityCache{};
    sharedVisibilityCache.reserve(4096u);
    for (CorridorPathCandidate& candidate : pathCandidates) {
        if (isCancelled()) {
            return std::nullopt;
        }
        bool pathIsValid = candidate.rawIsValid;
        std::vector<glm::vec3> candidateCorners =
            std::move(candidate.rawCorners);
        const bool corridorUsesLink = std::any_of(
            candidate.corridor->begin(),
            candidate.corridor->end(),
            [&](const NavCorridorStep& step) {
                return step.fromCellIndex < runtime.graph.size() &&
                    step.edgeIndex < runtime.graph[step.fromCellIndex].size() &&
                    runtime.graph[step.fromCellIndex][step.edgeIndex].viaLink;
            }
        );
        if (!pathIsValid && !profile.empty()) {
            if (corridorUsesLink) {
                if (std::optional<std::vector<glm::vec3>> linkedPath =
                        solveLinkedCorridorClearancePath(
                            runtime,
                            *endpoints,
                            *candidate.corridor,
                            profile,
                            cancelled);
                    linkedPath.has_value()) {
                    candidateCorners = std::move(*linkedPath);
                    pathIsValid = true;
                }
            } else {
                // Each corridor contributes only its own portals and boundary
                // nodes. Traversal itself is validated against the complete
                // authored surface, so identical directed segment checks are
                // safely reusable by the other corridor alternatives.
                if (std::optional<std::vector<glm::vec3>> clearancePath =
                        solveCorridorClearancePath(
                            runtime,
                            *endpoints,
                            *candidate.corridor,
                            profile,
                            cancelled,
                            true,
                            true,
                            &sharedVisibilityCache);
                    clearancePath.has_value() &&
                    pathSegmentsAreValid(
                        runtime,
                        endpoints->resolvedStart,
                        *clearancePath,
                        profile)) {
                    candidateCorners = std::move(*clearancePath);
                    pathIsValid = true;
                }
            }
        }
        if (!pathIsValid) {
            continue;
        }
        const float finalLength = pathLength(
            endpoints->resolvedStart,
            candidateCorners
        );
        if (finalLength < selectedLength) {
            selectedLength = finalLength;
            corners = std::move(candidateCorners);
        }
    }
    if (!profile.empty() && !corners.has_value()) {
        // If every corridor-local repair failed, retry once over the complete
        // planar component. This is a completeness fallback, not a path-length
        // heuristic; successful local solutions are compared above by their
        // validated final geometry.
        if (std::optional<std::vector<glm::vec3>> globalPath =
                solveCorridorClearancePath(
                    runtime,
                    *endpoints,
                    {},
                    profile,
                    cancelled,
                    true,
                    false,
                    &sharedVisibilityCache);
            globalPath.has_value() &&
            pathSegmentsAreValid(
                runtime,
                endpoints->resolvedStart,
                *globalPath,
                profile)) {
            const float globalLength = pathLength(
                endpoints->resolvedStart,
                *globalPath
            );
            if (globalLength < selectedLength) {
                selectedLength = globalLength;
                corners = std::move(*globalPath);
            }
        }
    }
    if (isCancelled()) {
        return std::nullopt;
    }
    if (exactPlanarCorners.has_value()) {
        const float exactPlanarLength = pathLength(
            endpoints->resolvedStart,
            *exactPlanarCorners
        );
        if (!corners.has_value() ||
            exactPlanarLength <= selectedLength + kPolygonEpsilon) {
            selectedLength = exactPlanarLength;
            corners = std::move(*exactPlanarCorners);
        }
    }
    if (!corners.has_value()) {
        return std::nullopt;
    }
    if (corners->size() == 1u &&
        glm::distance(endpoints->resolvedStart, corners->front()) <= arrivalRadius) {
        corners->clear();
    }
    return SolvedPath{
        endpoints->resolvedDestination,
        std::move(*corners),
        endpoints->resolvedStart
    };
}

}  // namespace core::navigation_detail
