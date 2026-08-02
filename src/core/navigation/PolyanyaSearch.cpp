#include "Polyanya.hpp"
#include "PolyanyaTopology.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <queue>
#include <vector>

#include <glm/geometric.hpp>

#include "polyanya/expansion.h"

namespace core::navigation_detail {
namespace {

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
        return lhs->interval.f == rhs->interval.f
            ? lhs->interval.g < rhs->interval.g
            : lhs->interval.f > rhs->interval.f;
    }
};

bool samePoint(const polyanya::Point& lhs, const polyanya::Point& rhs) {
    return lhs == rhs;
}

}  // namespace

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


}  // namespace core::navigation_detail

