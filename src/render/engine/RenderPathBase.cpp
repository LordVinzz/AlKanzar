#include "RenderPaths.hpp"

#include "core/profiling/ProfilerService.hpp"

namespace render {

void RenderPathBase::render(const RenderPathContext& context) {
    if (!beginFrame(context)) {
        return;
    }

    prepareTargets(context);
    drawGeometry(context);
    composeFrame(context);

    if (context.profiler) {
        ALKANZAR_PROFILE_SCOPE(*context.profiler, "Overlay Pass");
        context.overlayRenderer.renderOverlays(
            context.scene,
            context.lights,
            context.camera,
            context.options,
            context.width,
            context.height,
            context.directionalLightDirection
        );
    } else {
        context.overlayRenderer.renderOverlays(
            context.scene,
            context.lights,
            context.camera,
            context.options,
            context.width,
            context.height,
            context.directionalLightDirection
        );
    }
    context.overlayRenderer.renderPartySelectionMarquee(
        context.scene,
        context.width,
        context.height
    );
}

void RenderPathBase::drawStandardSceneLayers(
    const RenderPathContext& context,
    const SceneGeometryShaderContext& shaderContext
) const {
    const auto drawLayer = [&](const char* label, RenderLayer layer) {
        if (context.profiler) {
            ALKANZAR_PROFILE_SCOPE(*context.profiler, label);
            context.geometryRenderer.drawLayer(
                context.scene,
                layer,
                shaderContext,
                context.materialBinder,
                context.resources,
                context.jointTextureBuffer
            );
            return;
        }

        context.geometryRenderer.drawLayer(
            context.scene,
            layer,
            shaderContext,
            context.materialBinder,
            context.resources,
            context.jointTextureBuffer
        );
    };

    drawLayer("Ground Layer", RenderLayer::Ground);
    drawLayer("Geometry Layer", RenderLayer::Geometry);
    drawLayer("Actors Layer", RenderLayer::Actors);
}

}  // namespace render
