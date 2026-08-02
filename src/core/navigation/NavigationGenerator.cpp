#include "core/navigation/Navigation.hpp"
#include "core/navigation/NavigationDetailBake.hpp"
#include "core/navigation/NavigationDetailTypes.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <glm/geometric.hpp>

namespace core {

using namespace navigation_detail;

bool NavigationSystem::generateFromTags(const World& world, NavigationRuntime& runtime, std::string* error) const {
    std::vector<LayerBuildData> layers{};

    for (EntityId entity : world.navSources.entities()) {
        const NavSourceComponent& source = world.navSources.get(entity);
        const NavSourceGeometryComponent* geometry = world.navSourceGeometry.tryGet(entity);
        if (geometry == nullptr || !geometry->mesh) {
            continue;
        }
        if (entity.index >= world.transformCache_.size()) {
            continue;
        }
        const glm::mat4 worldMatrix = world.transformCache_[entity.index].worldMatrix;
        const render::Mesh& mesh = *geometry->mesh;
        if (mesh.indices.size() % 3u != 0u) {
            continue;
        }

        if (source.effectiveTag == NavSourceTag::Walkable) {
            for (std::size_t index = 0; index + 2u < mesh.indices.size(); index += 3u) {
                const unsigned int ia = mesh.indices[index];
                const unsigned int ib = mesh.indices[index + 1u];
                const unsigned int ic = mesh.indices[index + 2u];
                if (ia >= mesh.positions.size() || ib >= mesh.positions.size() || ic >= mesh.positions.size()) {
                    continue;
                }

                const glm::vec3 a = transformPoint3(worldMatrix, mesh.positions[ia]);
                const glm::vec3 b = transformPoint3(worldMatrix, mesh.positions[ib]);
                const glm::vec3 c = transformPoint3(worldMatrix, mesh.positions[ic]);
                const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
                if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z)) {
                    continue;
                }
                if (normal.y < kHorizontalNormalMinDot) {
                    continue;
                }
                const float elevation = (a.y + b.y + c.y) / 3.0f;
                addTriangleToLayer(
                    layers,
                    elevation,
                    WalkableTriangle{
                        elevation,
                        glm::vec2(a.x, a.z),
                        glm::vec2(b.x, b.z),
                        glm::vec2(c.x, c.z)
                    }
                );
            }
        }
    }

    std::vector<BlockingFootprint> blockers = buildBlockingFootprints(world);
    for (LayerBuildData& layer : layers) {
        layer.blockers = blockers;
    }

    runtime.asset.polygons.clear();
    runtime.asset.links.clear();
    int polygonId = 1;
    std::sort(layers.begin(), layers.end(), [](const LayerBuildData& lhs, const LayerBuildData& rhs) {
        return lhs.elevationY < rhs.elevationY;
    });
    for (const LayerBuildData& layer : layers) {
        std::vector<NavPolygon> polygons = buildPolygonsForLayer(layer, polygonId);
        runtime.asset.polygons.insert(
            runtime.asset.polygons.end(),
            std::make_move_iterator(polygons.begin()),
            std::make_move_iterator(polygons.end())
        );
    }

    if (!rebuildRuntimeInternal(runtime, error, true)) {
        return false;
    }
    setRuntimeStatus(
        runtime,
        "Generated " + std::to_string(runtime.asset.polygons.size()) + " navmesh polygons from hitbox unions.",
        false
    );
    return true;
}

}  // namespace core

