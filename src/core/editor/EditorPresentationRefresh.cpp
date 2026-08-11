#include "EditorUi.hpp"

#include "core/app/EngineServices.hpp"

namespace core {

void refreshEditorPresentation(EngineServices& services) {
    ALKANZAR_PROFILE_SCOPE(services.profiler, "Editor Presentation Refresh");
    if (services.world.transformsDirty()) {
        services.transformSystem.update(services.world, services.scheduler, false);
    }
    if (services.world.lightsDirty()) {
        services.lightSystem.update(services.world, services.time, services.scheduler, false);
    }
    services.renderExtractionSystem.extract(
        services.world,
        &services.editorSelection,
        services.frame,
        services.scheduler,
        false
    );
}

}  // namespace core
