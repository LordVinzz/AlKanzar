#include "core/navigation/NavigationDetailBake.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "core/transform/TransformMath.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace core::navigation_detail {

glm::vec3 transformPoint3(const glm::mat4& matrix, const glm::vec3& point) {
    return glm::vec3(matrix * glm::vec4(point, 1.0f));
}

EntityId objectRootEntity(const World& world, EntityId entity) {
    EntityId root = entity;
    while (const ParentComponent* parent = world.parents.tryGet(root)) {
        if (!parent->parent.valid() || !world.isAlive(parent->parent)) {
            break;
        }
        root = parent->parent;
    }
    return root;
}

const glm::mat4* cachedWorldMatrix(const World& world, EntityId entity) {
    if (entity.index >= world.transformCache_.size() || !world.transformCache_[entity.index].valid) {
        return nullptr;
    }
    return &world.transformCache_[entity.index].worldMatrix;
}

void appendBoxColliderFootprint(
    const BoxColliderComponent& collider,
    const glm::mat4& worldMatrix,
    std::vector<BlockingFootprint>& footprints
) {
    const OrientedBox box = makeOrientedBox(worldMatrix, collider);
    std::vector<glm::vec2> projectedCorners{};
    projectedCorners.reserve(box.corners.size());
    float minY = box.corners.front().y;
    float maxY = box.corners.front().y;
    for (const glm::vec3& corner : box.corners) {
        projectedCorners.emplace_back(corner.x, corner.z);
        minY = std::min(minY, corner.y);
        maxY = std::max(maxY, corner.y);
    }
    if (auto footprint = makeBlockingFootprint(buildConvexHull(std::move(projectedCorners)), minY, maxY)) {
        footprints.push_back(std::move(*footprint));
    }
}

void appendSphereColliderFootprint(
    const SphereColliderComponent& collider,
    const glm::mat4& worldMatrix,
    std::vector<BlockingFootprint>& footprints
) {
    constexpr int kSphereFootprintSides = 16;
    const glm::vec3 center = transformPoint3(worldMatrix, collider.center);
    const float maxScale = std::max({
        glm::length(glm::vec3(worldMatrix[0])),
        glm::length(glm::vec3(worldMatrix[1])),
        glm::length(glm::vec3(worldMatrix[2]))
    });
    const float radius = std::max(collider.radius * maxScale, kPolygonEpsilon);
    const float circumradius = radius / std::cos(3.141592653589793f / static_cast<float>(kSphereFootprintSides));

    std::vector<glm::vec2> vertices{};
    vertices.reserve(kSphereFootprintSides);
    for (int index = 0; index < kSphereFootprintSides; ++index) {
        const float angle = kTau * static_cast<float>(index) / static_cast<float>(kSphereFootprintSides);
        vertices.emplace_back(
            center.x + std::cos(angle) * circumradius,
            center.z + std::sin(angle) * circumradius
        );
    }
    if (auto footprint = makeBlockingFootprint(std::move(vertices), center.y - radius, center.y + radius)) {
        footprints.push_back(std::move(*footprint));
    }
}

std::vector<NavRuntimeCell> buildBoundedDefaultHitboxParts(std::vector<NavRuntimeCell> cells) {
    if (cells.size() <= kMaxProjectedCellsPerDefaultHitboxPart) {
        return cells;
    }

    std::vector<NavRuntimeCell> parts{};
    const auto partition = [&](const auto& self, std::vector<NavRuntimeCell> cluster) -> void {
        if (cluster.size() <= kMaxProjectedCellsPerDefaultHitboxPart) {
            std::vector<glm::vec2> points{};
            for (const NavRuntimeCell& cell : cluster) {
                points.insert(points.end(), cell.verticesXZ.begin(), cell.verticesXZ.end());
            }
            std::vector<glm::vec2> hull = buildConvexHull(std::move(points));
            if (polygonHasArea(hull)) {
                parts.push_back(NavRuntimeCell{0.0f, std::move(hull)});
            }
            return;
        }

        glm::vec2 minCenter(std::numeric_limits<float>::max());
        glm::vec2 maxCenter(std::numeric_limits<float>::lowest());
        for (const NavRuntimeCell& cell : cluster) {
            const glm::vec2 center = polygonCentroidXZ(cell.verticesXZ);
            minCenter = glm::min(minCenter, center);
            maxCenter = glm::max(maxCenter, center);
        }
        const bool splitX = maxCenter.x - minCenter.x >= maxCenter.y - minCenter.y;
        std::sort(cluster.begin(), cluster.end(), [splitX](const NavRuntimeCell& lhs, const NavRuntimeCell& rhs) {
            const glm::vec2 lhsCenter = polygonCentroidXZ(lhs.verticesXZ);
            const glm::vec2 rhsCenter = polygonCentroidXZ(rhs.verticesXZ);
            const float lhsPrimary = splitX ? lhsCenter.x : lhsCenter.y;
            const float rhsPrimary = splitX ? rhsCenter.x : rhsCenter.y;
            if (lhsPrimary != rhsPrimary) {
                return lhsPrimary < rhsPrimary;
            }
            return (splitX ? lhsCenter.y : lhsCenter.x) < (splitX ? rhsCenter.y : rhsCenter.x);
        });

        const std::size_t middle = cluster.size() / 2u;
        std::vector<NavRuntimeCell> rhs(
            std::make_move_iterator(cluster.begin() + static_cast<std::ptrdiff_t>(middle)),
            std::make_move_iterator(cluster.end())
        );
        cluster.erase(cluster.begin() + static_cast<std::ptrdiff_t>(middle), cluster.end());
        self(self, std::move(cluster));
        self(self, std::move(rhs));
    };

    partition(partition, std::move(cells));
    return parts;
}

void appendDefaultGeometryFootprints(
    const render::Mesh& mesh,
    const glm::mat4& worldMatrix,
    std::vector<BlockingFootprint>& footprints
) {
    if (mesh.positions.empty()) {
        return;
    }

    std::vector<glm::vec3> worldPositions{};
    worldPositions.reserve(mesh.positions.size());
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    for (const glm::vec3& position : mesh.positions) {
        const glm::vec3 worldPosition = transformPoint3(worldMatrix, position);
        worldPositions.push_back(worldPosition);
        minY = std::min(minY, worldPosition.y);
        maxY = std::max(maxY, worldPosition.y);
    }

    std::unordered_set<std::string> emitted{};
    std::vector<NavRuntimeCell> projectedCells{};
    if (!mesh.indices.empty() && mesh.indices.size() % 3u == 0u) {
        for (std::size_t index = 0; index < mesh.indices.size(); index += 3u) {
            const unsigned int ia = mesh.indices[index];
            const unsigned int ib = mesh.indices[index + 1u];
            const unsigned int ic = mesh.indices[index + 2u];
            if (ia >= worldPositions.size() || ib >= worldPositions.size() || ic >= worldPositions.size()) {
                continue;
            }
            std::vector<glm::vec2> triangle{
                glm::vec2(worldPositions[ia].x, worldPositions[ia].z),
                glm::vec2(worldPositions[ib].x, worldPositions[ib].z),
                glm::vec2(worldPositions[ic].x, worldPositions[ic].z),
            };
            const std::string key = canonicalPolygonKey(triangle);
            if (key.empty() || !emitted.insert(key).second) {
                continue;
            }
            triangle = normalizePolygonVertices(triangle);
            if (polygonHasArea(triangle)) {
                projectedCells.push_back(NavRuntimeCell{0.0f, std::move(triangle)});
            }
        }
        if (!emitted.empty()) {
            mergeAdjacentConvexCells(projectedCells);
            projectedCells = buildBoundedDefaultHitboxParts(std::move(projectedCells));
            for (NavRuntimeCell& cell : projectedCells) {
                if (auto footprint = makeBlockingFootprint(std::move(cell.verticesXZ), minY, maxY)) {
                    footprints.push_back(std::move(*footprint));
                }
            }
            return;
        }
    }

    std::vector<glm::vec2> projected{};
    projected.reserve(worldPositions.size());
    for (const glm::vec3& position : worldPositions) {
        projected.emplace_back(position.x, position.z);
    }
    if (auto footprint = makeBlockingFootprint(buildConvexHull(std::move(projected)), minY, maxY)) {
        footprints.push_back(std::move(*footprint));
    }
}

std::vector<BlockingFootprint> buildBlockingFootprints(const World& world) {
    std::unordered_map<EntityId, std::vector<EntityId>> blockingSourcesByRoot{};
    for (EntityId entity : world.navSources.entities()) {
        if (world.navSources.get(entity).effectiveTag == NavSourceTag::Blocking) {
            blockingSourcesByRoot[objectRootEntity(world, entity)].push_back(entity);
        }
    }

    std::unordered_map<EntityId, std::vector<BlockingFootprint>> explicitFootprintsByRoot{};
    for (EntityId entity : world.boxColliders.entities()) {
        const BoxColliderComponent& collider = world.boxColliders.get(entity);
        const EntityId root = objectRootEntity(world, entity);
        const glm::mat4* worldMatrix = cachedWorldMatrix(world, entity);
        if (collider.isTrigger || worldMatrix == nullptr || !blockingSourcesByRoot.contains(root)) {
            continue;
        }
        appendBoxColliderFootprint(collider, *worldMatrix, explicitFootprintsByRoot[root]);
    }
    for (EntityId entity : world.sphereColliders.entities()) {
        const SphereColliderComponent& collider = world.sphereColliders.get(entity);
        const EntityId root = objectRootEntity(world, entity);
        const glm::mat4* worldMatrix = cachedWorldMatrix(world, entity);
        if (collider.isTrigger || worldMatrix == nullptr || !blockingSourcesByRoot.contains(root)) {
            continue;
        }
        appendSphereColliderFootprint(collider, *worldMatrix, explicitFootprintsByRoot[root]);
    }

    std::vector<BlockingFootprint> footprints{};
    for (const auto& [root, sources] : blockingSourcesByRoot) {
        auto explicitIt = explicitFootprintsByRoot.find(root);
        if (explicitIt != explicitFootprintsByRoot.end() && !explicitIt->second.empty()) {
            footprints.insert(
                footprints.end(),
                std::make_move_iterator(explicitIt->second.begin()),
                std::make_move_iterator(explicitIt->second.end())
            );
            continue;
        }

        for (EntityId sourceEntity : sources) {
            const NavSourceGeometryComponent* geometry = world.navSourceGeometry.tryGet(sourceEntity);
            const glm::mat4* worldMatrix = cachedWorldMatrix(world, sourceEntity);
            if (geometry == nullptr || !geometry->mesh || worldMatrix == nullptr) {
                continue;
            }
            appendDefaultGeometryFootprints(*geometry->mesh, *worldMatrix, footprints);
        }
    }
    return footprints;
}

void appendConvexPolygonToUnion(
    std::vector<NavRuntimeCell>& cells,
    std::vector<glm::vec2> polygon,
    float elevationY
) {
    polygon = normalizePolygonVertices(polygon);
    if (!polygonHasArea(polygon)) {
        return;
    }

    std::vector<std::vector<glm::vec2>> additions{std::move(polygon)};
    for (const NavRuntimeCell& existing : cells) {
        const auto existingFootprint = makeBlockingFootprint(existing.verticesXZ, elevationY, elevationY);
        if (!existingFootprint.has_value()) {
            continue;
        }
        std::vector<std::vector<glm::vec2>> next{};
        for (const std::vector<glm::vec2>& addition : additions) {
            std::vector<std::vector<glm::vec2>> pieces = subtractConvexPolygon(addition, *existingFootprint);
            next.insert(next.end(), std::make_move_iterator(pieces.begin()), std::make_move_iterator(pieces.end()));
        }
        additions = std::move(next);
        if (additions.empty()) {
            return;
        }
    }

    for (std::vector<glm::vec2>& addition : additions) {
        cells.push_back(NavRuntimeCell{elevationY, std::move(addition)});
    }
}

std::optional<std::vector<NavPolygon>> buildPolygonsForLayer(
    const LayerBuildData& layer,
    int& nextPolygonId,
    float maximumPolygonEdgeLength,
    float minimumTriangleArea,
    std::string* error
) {
    std::vector<NavRuntimeCell> cells{};
    for (const WalkableTriangle& triangle : layer.triangles) {
        appendConvexPolygonToUnion(cells, {triangle.a, triangle.b, triangle.c}, layer.elevationY);
    }

    mergeAdjacentConvexCells(cells);
    std::vector<NavRuntimeCell> blockerCells{};
    for (const BlockingFootprint& blocker : layer.blockers) {
        if (!blockerOverlapsLayer(blocker, layer.elevationY)) {
            continue;
        }
        appendConvexPolygonToUnion(blockerCells, blocker.verticesXZ, layer.elevationY);
    }
    mergeAdjacentConvexCells(blockerCells);

    for (const NavRuntimeCell& blockerCell : blockerCells) {
        const auto blocker = makeBlockingFootprint(
            blockerCell.verticesXZ,
            layer.elevationY,
            layer.elevationY
        );
        if (!blocker.has_value()) {
            continue;
        }
        std::vector<NavRuntimeCell> next{};
        for (const NavRuntimeCell& cell : cells) {
            std::vector<std::vector<glm::vec2>> pieces = subtractConvexPolygon(cell.verticesXZ, *blocker);
            for (std::vector<glm::vec2>& piece : pieces) {
                next.push_back(NavRuntimeCell{layer.elevationY, std::move(piece)});
            }
        }
        cells = std::move(next);
        if (cells.empty()) {
            break;
        }
    }
    mergeAdjacentConvexCells(cells);

    std::string delaunayError{};
    const auto triangulated = triangulateWalkableCellsDelaunay(
        cells,
        layer.elevationY,
        &delaunayError
    );
    if (!triangulated.has_value()) {
        if (error != nullptr) {
            *error = "Constrained Delaunay triangulation failed: " +
                (delaunayError.empty() ? "unknown error" : delaunayError);
        }
        return std::nullopt;
    }
    cells = *triangulated;

    if (maximumPolygonEdgeLength > 0.0f) {
        const auto constrained = enforceMaximumPolygonEdgeLength(
            cells,
            maximumPolygonEdgeLength
        );
        if (!constrained.has_value()) {
            if (error != nullptr) {
                *error = "Maximum Polygon Edge Length generates too many navmesh cells.";
            }
            return std::nullopt;
        }
        cells = *constrained;
    }
    cells = applyMinimumTriangleAreaConstraint(cells, minimumTriangleArea);
    if (cells.empty()) {
        if (error != nullptr) {
            *error = "Minimum Generated Triangle Area removed every triangle from a navmesh layer.";
        }
        return std::nullopt;
    }

    std::vector<NavPolygon> polygons{};
    polygons.reserve(cells.size());
    for (NavRuntimeCell& cell : cells) {
        polygons.push_back(NavPolygon{nextPolygonId++, layer.elevationY, std::move(cell.verticesXZ)});
    }
    return polygons;
}

void addTriangleToLayer(
    std::vector<LayerBuildData>& layers,
    float elevationY,
    const WalkableTriangle& triangle
) {
    for (LayerBuildData& layer : layers) {
        if (std::abs(layer.elevationY - elevationY) <= kLayerGroupingEpsilon) {
            layer.triangles.push_back(triangle);
            return;
        }
    }
    layers.push_back(LayerBuildData{elevationY, {triangle}, {}});
}



}  // namespace core::navigation_detail
