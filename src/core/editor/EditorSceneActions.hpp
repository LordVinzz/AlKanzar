#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/scene/SceneDocument.hpp"

namespace core {

struct EngineServices;
enum class SceneDocumentAction;

struct EditorSceneSnapshot {
    SceneBlueprint blueprint{};
    std::optional<SceneObjectId> selectedObject{};
};

[[nodiscard]] std::optional<SceneObjectId> selectedAuthoredObject(
    const EngineServices& services
);

[[nodiscard]] bool rebuildEditorScene(
    EngineServices& services,
    std::optional<SceneObjectId> selectedObject,
    std::string* error = nullptr
);

void executeEditorSceneSnapshot(
    EngineServices& services,
    std::string label,
    EditorSceneSnapshot before,
    EditorSceneSnapshot after
);

void setEditorSceneStatus(EngineServices& services, std::string message, bool isError);
void requestSceneDocumentAction(EngineServices& services, SceneDocumentAction action);
void beginEditorSceneDocumentAction(EngineServices& services, SceneDocumentAction action);

[[nodiscard]] bool createEditorSceneObject(
    EngineServices& services,
    SceneObjectType type,
    std::string_view modelAsset = "Adventurer.glb",
    ScenePrimitiveShape primitiveShape = ScenePrimitiveShape::Box
);
[[nodiscard]] bool duplicateEditorSceneObject(EngineServices& services, std::string_view id);
[[nodiscard]] bool deleteEditorSceneObject(EngineServices& services, std::string_view id);
[[nodiscard]] bool reparentEditorSceneObject(
    EngineServices& services,
    std::string_view id,
    std::optional<SceneObjectId> parentId
);

}  // namespace core
