#include "EditorSceneActions.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "Command.hpp"
#include "core/app/EngineServices.hpp"
#include "core/scene/SceneAsset.hpp"

namespace core {
namespace {

void applyEditorSceneSnapshot(EngineServices& services, const EditorSceneSnapshot& snapshot) {
    services.sceneDocument.replaceBlueprint(snapshot.blueprint);
    std::string error{};
    if (!rebuildEditorScene(services, snapshot.selectedObject, &error)) {
        setEditorSceneStatus(services, std::move(error), true);
    }
}

}  // namespace

std::optional<SceneObjectId> selectedAuthoredObject(const EngineServices& services) {
    const std::optional<SelectionTarget>& selection = services.editorSelection.current();
    if (!selection.has_value()) {
        return std::nullopt;
    }
    return services.sceneDocument.objectForEntity(selection->entity);
}

bool rebuildEditorScene(
    EngineServices& services,
    std::optional<SceneObjectId> selectedObject,
    std::string* error
) {
    services.editorSelection.clear();
    services.partySelection.clear();
    services.frame.clear();
    if (!services.renderer.resetSceneResources()) {
        if (error != nullptr) {
            *error = "The renderer could not reset scene-owned resources.";
        }
        services.sceneLoaded = false;
        return false;
    }
    services.sceneLoaded = services.sceneFactory.buildScene(
        services.sceneDocument.blueprint(),
        services.world,
        services.renderer
    );
    if (!services.sceneLoaded) {
        if (error != nullptr) {
            *error = "The scene document is valid, but its runtime resources could not be rebuilt.";
        }
        return false;
    }

    services.sceneDocument.bindWorld(services.world);
    if (!services.navigationSystem.initializeScene(
            services.sceneDocument.blueprint(),
            services.world,
            services.navigation)) {
        spdlog::warn("Editor: scene rebuilt with navigation warning: {}", services.navigation.statusMessage);
    }
    const std::vector<EntityId> partyMembers =
        services.partySelectionSystem.orderedActivePartyMembers(services.world);
    if (!partyMembers.empty()) {
        services.partySelection.setLeader(partyMembers.front());
    }
    if (selectedObject.has_value()) {
        services.editorSelection.set(services.sceneDocument.entityForObject(*selectedObject));
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void executeEditorSceneSnapshot(
    EngineServices& services,
    std::string label,
    EditorSceneSnapshot before,
    EditorSceneSnapshot after
) {
    std::string serialized{};
    std::string validationError{};
    if (!serializeSceneAsset(after.blueprint, serialized, &validationError)) {
        setEditorSceneStatus(
            services,
            "Scene edit rejected: " + validationError,
            true
        );
        return;
    }
    services.commands.execute(std::make_unique<SnapshotCommand<EditorSceneSnapshot>>(
        std::move(label),
        std::string{},
        std::move(before),
        std::move(after),
        [&services](const EditorSceneSnapshot& snapshot) {
            applyEditorSceneSnapshot(services, snapshot);
        },
        false
    ));
}

void setEditorSceneStatus(EngineServices& services, std::string message, bool isError) {
    services.editorSession.sceneDocumentStatus = std::move(message);
    services.editorSession.sceneDocumentStatusIsError = isError;
}

void requestSceneDocumentAction(EngineServices& services, SceneDocumentAction action) {
    services.editorSession.pendingSceneAction = action;
    if (services.sceneDocument.dirty()) {
        services.editorSession.unsavedChangesDialogRequested = true;
        return;
    }
    switch (action) {
        case SceneDocumentAction::OpenScene:
            services.editorSession.openSceneDialogRequested = true;
            break;
        case SceneDocumentAction::NewScene:
        case SceneDocumentAction::ReloadScene:
        case SceneDocumentAction::Quit:
        case SceneDocumentAction::None:
            break;
    }
}

}  // namespace core
