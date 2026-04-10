#include "RenderSceneView.hpp"

#include <algorithm>

#include "core/systems/TaskScheduler.hpp"

namespace render {

namespace {

std::size_t taskGrain(std::size_t count, std::size_t workerCount) {
    const std::size_t lanes = std::max<std::size_t>(1u, workerCount + 1u) * 2u;
    return std::max<std::size_t>(32u, (count + lanes - 1u) / lanes);
}

}  // namespace

RenderSelectionView resolveRenderSelection(const core::FrameSceneData& frame) {
    RenderSelectionView selection{};
    if (!frame.selection.entity.has_value()) {
        return selection;
    }

    selection.worldBounds = frame.selection.worldBounds;
    selection.hasWorldBounds = frame.selection.hasWorldBounds;
    selection.transformMatrix = frame.selection.transformMatrix;

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
    scene.selection = resolveRenderSelection(frame);
    scene.selectionSkeleton.showOverlay = frame.selectionSkeleton.showOverlay;
    scene.selectionSkeleton.parentIndices = frame.selectionSkeleton.parentIndices;
    scene.selectionSkeleton.jointWorldPositions = frame.selectionSkeleton.jointWorldPositions;

    if (!useParallel) {
        scene.objects.reserve(frame.renderables.size());
        for (std::size_t index = 0; index < frame.renderables.size(); ++index) {
            const core::FrameRenderable& renderable = frame.renderables[index];

            const MeshBuffer* mesh = nullptr;
            if (renderable.mesh.valid() && renderable.mesh.value < meshLookup.size()) {
                mesh = meshLookup[renderable.mesh.value];
            }

            RenderSceneObjectView object{};
            object.sourceIndex = static_cast<int>(index);
            object.mesh = mesh;
            object.material = renderable.material;
            object.layer = renderable.layer;
            object.localBounds = renderable.localBounds;
            object.worldBounds = renderable.worldBounds;
            object.modelMatrix = renderable.modelMatrix;
            object.skinned = renderable.skinned;
            object.jointMatrixBase = renderable.jointMatrixBase;
            object.jointMatrixCount = renderable.jointMatrixCount;
            object.visible = renderable.visible;
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
                    object.sourceIndex = static_cast<int>(index);
                    object.mesh = mesh;
                    object.material = renderable.material;
                    object.layer = renderable.layer;
                    object.localBounds = renderable.localBounds;
                    object.worldBounds = renderable.worldBounds;
                    object.modelMatrix = renderable.modelMatrix;
                    object.skinned = renderable.skinned;
                    object.jointMatrixBase = renderable.jointMatrixBase;
                    object.jointMatrixCount = renderable.jointMatrixCount;
                    object.visible = renderable.visible;
                    scene.objects[index] = std::move(object);
                }
            }
        );
        scheduler.wait(buildGroup);
    }

    return scene;
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
