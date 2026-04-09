#include "RenderSceneView.hpp"

namespace render {

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

RenderSceneView buildRenderSceneView(const core::FrameSceneData& frame, const std::vector<const MeshBuffer*>& meshLookup) {
    RenderSceneView scene{};
    scene.objects.reserve(frame.renderables.size());
    scene.lights = frame.lights;
    scene.lightVolumes = frame.lightVolumes;
    scene.selection = resolveRenderSelection(frame);

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
        object.visible = renderable.visible;
        scene.objects.push_back(std::move(object));
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
        });
    }
    return renderables;
}

}  // namespace render
