#include "RenderSceneView.hpp"

#include <algorithm>
#include <array>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include "core/systems/TaskScheduler.hpp"

namespace render {

namespace {

std::size_t taskGrain(std::size_t count, std::size_t workerCount) {
    const std::size_t lanes = std::max<std::size_t>(1u, workerCount + 1u) * 2u;
    return std::max<std::size_t>(32u, (count + lanes - 1u) / lanes);
}

struct FrustumPlane {
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float distance{0.0f};
};

FrustumPlane normalizePlane(const glm::vec4& plane) {
    const glm::vec3 normal(plane);
    const float length = glm::length(normal);
    if (length <= 1.0e-6f) {
        return FrustumPlane{};
    }

    return FrustumPlane{
        normal / length,
        plane.w / length,
    };
}

std::array<FrustumPlane, 6> buildFrustumPlanes(const CameraMatrices& camera) {
    const glm::mat4 rows = glm::transpose(camera.projection * camera.view);
    return {
        normalizePlane(rows[3] + rows[0]),
        normalizePlane(rows[3] - rows[0]),
        normalizePlane(rows[3] + rows[1]),
        normalizePlane(rows[3] - rows[1]),
        normalizePlane(rows[3] + rows[2]),
        normalizePlane(rows[3] - rows[2]),
    };
}

bool boundsInsideFrustum(const Bounds3& bounds, const std::array<FrustumPlane, 6>& planes) {
    for (const FrustumPlane& plane : planes) {
        const glm::vec3 positiveVertex(
            plane.normal.x >= 0.0f ? bounds.max.x : bounds.min.x,
            plane.normal.y >= 0.0f ? bounds.max.y : bounds.min.y,
            plane.normal.z >= 0.0f ? bounds.max.z : bounds.min.z
        );
        if (glm::dot(plane.normal, positiveVertex) + plane.distance < 0.0f) {
            return false;
        }
    }
    return true;
}

}  // namespace

RenderSelectionView resolveRenderSelection(const core::FrameSceneData& frame) {
    RenderSelectionView selection{};
    if (!frame.selection.entity.has_value()) {
        return selection;
    }

    selection.entity = *frame.selection.entity;
    selection.worldBounds = frame.selection.worldBounds;
    selection.hasWorldBounds = frame.selection.hasWorldBounds;
    selection.transformMatrix = frame.selection.transformMatrix;
    selection.boundsModelMatrix = frame.selection.boundsModelMatrix;
    selection.hasBoundsModelMatrix = frame.selection.hasBoundsModelMatrix;

    for (std::size_t index = 0; index < frame.renderables.size(); ++index) {
        if (frame.renderables[index].entity == *frame.selection.entity) {
            selection.kind = RenderSelectionKind::Renderable;
            selection.index = static_cast<int>(index);
            return selection;
        }
    }

    for (std::size_t index = 0; index < frame.lights.size(); ++index) {
        if (frame.lights[index].entity == *frame.selection.entity) {
            selection.kind = RenderSelectionKind::Light;
            selection.index = static_cast<int>(index);
            return selection;
        }
    }

    selection.kind = RenderSelectionKind::Node;
    return selection;
}

RenderSceneView buildRenderSceneView(
    const core::FrameSceneData& frame,
    const std::vector<const MeshBuffer*>& meshLookup,
    core::TaskScheduler& scheduler,
    bool useParallel
) {
    RenderSceneView scene{};
    scene.lights = frame.lights;
    scene.lightVolumes = frame.lightVolumes;
    scene.colliderDebug.reserve(frame.colliderDebug.size());
    for (const core::FrameColliderDebug& collider : frame.colliderDebug) {
        scene.colliderDebug.push_back(RenderColliderDebugView{
            collider.entity,
            collider.shape,
            collider.bounds,
            collider.modelMatrix,
            collider.center,
            collider.radius
        });
    }
    scene.selection = resolveRenderSelection(frame);
    scene.selectionSkeleton.showOverlay = frame.selectionSkeleton.showOverlay;
    scene.selectionSkeleton.parentIndices = frame.selectionSkeleton.parentIndices;
    scene.selectionSkeleton.jointWorldPositions = frame.selectionSkeleton.jointWorldPositions;
    scene.navigation.path = frame.navigation.path;
    scene.navigation.destination = frame.navigation.destination;
    scene.navigation.captureVertices = frame.navigation.captureVertices;
    scene.navigation.captureElevationY = frame.navigation.captureElevationY;
    scene.navigation.polygons.reserve(frame.navigation.polygons.size());
    scene.navigation.links.reserve(frame.navigation.links.size());
    for (const core::FrameNavDebugPolygon& polygon : frame.navigation.polygons) {
        scene.navigation.polygons.push_back(RenderNavPolygonView{
            polygon.id,
            polygon.elevationY,
            polygon.vertices,
            polygon.color
        });
    }
    for (const core::FrameNavDebugLink& link : frame.navigation.links) {
        scene.navigation.links.push_back(RenderNavLinkView{
            link.id,
            link.fromPoint,
            link.toPoint,
            link.bidirectional
        });
    }

    if (!useParallel) {
        scene.objects.reserve(frame.renderables.size());
        for (std::size_t index = 0; index < frame.renderables.size(); ++index) {
            const core::FrameRenderable& renderable = frame.renderables[index];

            const MeshBuffer* mesh = nullptr;
            if (renderable.mesh.valid() && renderable.mesh.value < meshLookup.size()) {
                mesh = meshLookup[renderable.mesh.value];
            }

            RenderSceneObjectView object{};
            object.entity = renderable.entity;
            object.sourceIndex = static_cast<int>(index);
            object.mesh = mesh;
            object.material = renderable.material;
            object.layer = renderable.layer;
            object.localBounds = renderable.localBounds;
            object.worldBounds = renderable.worldBounds;
            object.hasWorldBounds = renderable.hasWorldBounds;
            object.modelMatrix = renderable.modelMatrix;
            object.skinned = renderable.skinned;
            object.jointMatrixBase = renderable.jointMatrixBase;
            object.jointMatrixCount = renderable.jointMatrixCount;
            object.visible = renderable.visible;
            object.frustumVisible = true;
            object.occlusionVisible = true;
            scene.objects.push_back(std::move(object));
        }
    } else {
        scene.objects.resize(frame.renderables.size());

        core::TaskGroup buildGroup;
        scheduler.parallelFor(
            buildGroup,
            frame.renderables.size(),
            taskGrain(frame.renderables.size(), scheduler.workerCount()),
            "Scene Object Build",
            [&](std::size_t begin, std::size_t end) {
                for (std::size_t index = begin; index < end; ++index) {
                    const core::FrameRenderable& renderable = frame.renderables[index];

                    const MeshBuffer* mesh = nullptr;
                    if (renderable.mesh.valid() && renderable.mesh.value < meshLookup.size()) {
                        mesh = meshLookup[renderable.mesh.value];
                    }

                    RenderSceneObjectView object{};
                    object.entity = renderable.entity;
                    object.sourceIndex = static_cast<int>(index);
                    object.mesh = mesh;
                    object.material = renderable.material;
                    object.layer = renderable.layer;
                    object.localBounds = renderable.localBounds;
                    object.worldBounds = renderable.worldBounds;
                    object.hasWorldBounds = renderable.hasWorldBounds;
                    object.modelMatrix = renderable.modelMatrix;
                    object.skinned = renderable.skinned;
                    object.jointMatrixBase = renderable.jointMatrixBase;
                    object.jointMatrixCount = renderable.jointMatrixCount;
                    object.visible = renderable.visible;
                    object.frustumVisible = true;
                    object.occlusionVisible = true;
                    scene.objects[index] = std::move(object);
                }
            }
        );
        scheduler.wait(buildGroup);
    }

    return scene;
}

void applyCameraFrustumCulling(RenderSceneView& scene, const CameraMatrices& camera) {
    scene.frustumCullStats = FrustumCullStats{};
    scene.frustumCullStats.totalRenderables = static_cast<std::uint32_t>(scene.objects.size());
    const std::array<FrustumPlane, 6> planes = buildFrustumPlanes(camera);

    for (RenderSceneObjectView& object : scene.objects) {
        object.frustumVisible = true;
        if (!object.visible) {
            scene.frustumCullStats.visibilityHidden++;
            continue;
        }
        if (!object.hasWorldBounds) {
            scene.frustumCullStats.noBoundsBypass++;
            scene.frustumCullStats.mainPassVisible++;
            continue;
        }

        scene.frustumCullStats.boundsTested++;
        object.frustumVisible = boundsInsideFrustum(object.worldBounds, planes);
        if (object.frustumVisible) {
            scene.frustumCullStats.frustumPassed++;
            scene.frustumCullStats.mainPassVisible++;
        } else {
            scene.frustumCullStats.frustumCulled++;
        }
    }
}

void applyLastKnownOcclusionVisibility(
    RenderSceneView& scene,
    const std::unordered_map<core::EntityId, OcclusionCullCacheState>& cache
) {
    scene.occlusionCullStats = OcclusionCullStats{};
    for (RenderSceneObjectView& object : scene.objects) {
        object.occlusionVisible = true;
        if (!object.visible || !object.frustumVisible || object.layer == RenderLayer::Ground) {
            continue;
        }
        if (!object.hasWorldBounds) {
            scene.occlusionCullStats.noBoundsBypass++;
            continue;
        }

        scene.occlusionCullStats.candidates++;
        const auto it = cache.find(object.entity);
        if (it == cache.end() || !it->second.hasLastResult) {
            scene.occlusionCullStats.warmupVisible++;
            scene.occlusionCullStats.visible++;
            continue;
        }

        // Require more than one consecutive occluded result before hiding to avoid
        // one-frame flicker from unstable hardware query results on thin geometry.
        object.occlusionVisible = it->second.lastVisible || it->second.occludedFrameStreak < 2u;
        if (it->second.queryInFlight) {
            scene.occlusionCullStats.pendingReused++;
        }
        if (object.occlusionVisible) {
            scene.occlusionCullStats.visible++;
        } else {
            scene.occlusionCullStats.occluded++;
        }
    }
}

std::vector<ShadowSystem::ShadowRenderable> collectShadowRenderables(const RenderSceneView& scene) {
    std::vector<ShadowSystem::ShadowRenderable> renderables;
    renderables.reserve(scene.objects.size());
    for (const RenderSceneObjectView& object : scene.objects) {
        if (!object.visible || object.mesh == nullptr || !object.mesh->valid()) {
            continue;
        }
        renderables.push_back(ShadowSystem::ShadowRenderable{
            object.mesh,
            object.modelMatrix,
            object.skinned,
            object.jointMatrixBase,
            object.jointMatrixCount,
        });
    }
    return renderables;
}

}  // namespace render
