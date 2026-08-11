#include "EditorSceneActions.hpp"

#include <optional>
#include <string>
#include <string_view>

#include "core/app/EngineServices.hpp"
#include "core/scene/SceneDocument.hpp"

namespace core {
namespace {

SceneDocument draftDocument(const EngineServices& services) {
    SceneDocument draft{};
    draft.begin(services.sceneDocument.blueprint());
    return draft;
}

EditorSceneSnapshot currentSnapshot(const EngineServices& services) {
    return EditorSceneSnapshot{
        services.sceneDocument.blueprint(),
        selectedAuthoredObject(services)
    };
}

}  // namespace

bool createEditorSceneObject(
    EngineServices& services,
    SceneObjectType type,
    std::string_view modelAsset,
    ScenePrimitiveShape primitiveShape
) {
    const EditorSceneSnapshot before = currentSnapshot(services);
    SceneDocument draft = draftDocument(services);
    const std::optional<SceneObjectId> created = draft.addDefaultObject(
        type,
        modelAsset,
        primitiveShape
    );
    if (!created.has_value()) {
        setEditorSceneStatus(
            services,
            type == SceneObjectType::DirectionalLight
                ? "The scene already contains its single directional light."
                : "The authored object could not be created.",
            true
        );
        return false;
    }
    executeEditorSceneSnapshot(
        services,
        "Create Scene Object",
        before,
        EditorSceneSnapshot{draft.blueprint(), created}
    );
    return true;
}

bool duplicateEditorSceneObject(EngineServices& services, std::string_view id) {
    const EditorSceneSnapshot before = currentSnapshot(services);
    SceneDocument draft = draftDocument(services);
    const std::optional<SceneObjectId> duplicate = draft.duplicateObject(id);
    if (!duplicate.has_value()) {
        setEditorSceneStatus(services, "This authored object cannot be duplicated.", true);
        return false;
    }
    executeEditorSceneSnapshot(
        services,
        "Duplicate Scene Object",
        before,
        EditorSceneSnapshot{draft.blueprint(), duplicate}
    );
    return true;
}

bool deleteEditorSceneObject(EngineServices& services, std::string_view id) {
    const EditorSceneSnapshot before = currentSnapshot(services);
    SceneDocument draft = draftDocument(services);
    if (!draft.deleteObjectRecursive(id)) {
        setEditorSceneStatus(services, "Only authored SCN objects can be deleted.", true);
        return false;
    }
    executeEditorSceneSnapshot(
        services,
        "Delete Scene Object",
        before,
        EditorSceneSnapshot{draft.blueprint(), std::nullopt}
    );
    return true;
}

bool reparentEditorSceneObject(
    EngineServices& services,
    std::string_view id,
    std::optional<SceneObjectId> parentId
) {
    const EditorSceneSnapshot before = currentSnapshot(services);
    SceneDocument draft = draftDocument(services);
    std::string error{};
    if (!draft.reparentObject(id, std::move(parentId), true, &error)) {
        setEditorSceneStatus(services, std::move(error), true);
        return false;
    }
    executeEditorSceneSnapshot(
        services,
        "Reparent Scene Object",
        before,
        EditorSceneSnapshot{draft.blueprint(), SceneObjectId(id)}
    );
    return true;
}

}  // namespace core
