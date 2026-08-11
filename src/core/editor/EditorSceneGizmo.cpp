#include "core/editor/EditorUi.hpp"

#include <optional>
#include <string>

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

#include "Command.hpp"
#include "EditorGizmoMath.hpp"
#include "EditorSceneActions.hpp"
#include "core/app/EngineServices.hpp"
#include "core/scene/Camera.hpp"

namespace core {
namespace {

const char* gizmoCommandLabel(EditorGizmoOperation operation) {
    switch (operation) {
        case EditorGizmoOperation::Translate: return "Move Scene Object";
        case EditorGizmoOperation::Rotate: return "Rotate Scene Object";
        case EditorGizmoOperation::Scale: return "Scale Scene Object";
    }
    return "Transform Scene Object";
}

ImGuizmo::OPERATION imGuizmoOperation(EditorGizmoOperation operation) {
    switch (operation) {
        case EditorGizmoOperation::Translate: return ImGuizmo::TRANSLATE;
        case EditorGizmoOperation::Rotate: return ImGuizmo::ROTATE;
        case EditorGizmoOperation::Scale: return ImGuizmo::SCALE;
    }
    return ImGuizmo::TRANSLATE;
}

void applyAuthoredTransform(
    EngineServices& services,
    const SceneObjectId& objectId,
    const TransformComponent& snapshot
) {
    const std::optional<EntityId> entity = services.sceneDocument.entityForObject(objectId);
    if (!entity.has_value()) return;
    if (TransformComponent* transform = services.world.transforms.tryGet(*entity)) {
        *transform = snapshot;
        transform->scale = glm::max(transform->scale, glm::vec3(0.01f));
        notifyEditorTransformChanged(services, *entity);
    }
}

void finishGizmoCommand(EngineServices& services) {
    EditorSession& session = services.editorSession;
    if (!session.gizmoWasUsing || session.gizmoActiveObjectId.empty()) {
        session.gizmoWasUsing = false;
        session.gizmoActiveObjectId.clear();
        return;
    }
    const SceneObjectId objectId = session.gizmoActiveObjectId;
    const std::optional<EntityId> entity = services.sceneDocument.entityForObject(objectId);
    if (entity.has_value()) {
        if (const TransformComponent* transform = services.world.transforms.tryGet(*entity)) {
            services.commands.execute(std::make_unique<SnapshotCommand<TransformComponent>>(
                gizmoCommandLabel(session.gizmoOperation),
                "gizmo-" + objectId + "-" + std::to_string(static_cast<int>(session.gizmoOperation)),
                session.gizmoStartTransform,
                *transform,
                [&services, objectId](const TransformComponent& snapshot) {
                    applyAuthoredTransform(services, objectId, snapshot);
                }
            ));
        }
    }
    session.gizmoWasUsing = false;
    session.gizmoActiveObjectId.clear();
}

void drawGizmoToolbar(EngineServices& services, bool hasTransform, bool worldRotationBlocked) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + 12.0f),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.0f)
    );
    ImGui::SetNextWindowBgAlpha(0.88f);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav;
    if (!ImGui::Begin("###SceneGizmoToolbar", nullptr, flags)) {
        ImGui::End();
        return;
    }
    if (!hasTransform) ImGui::BeginDisabled();
    if (ImGui::RadioButton("Move (G)", services.editorSession.gizmoOperation == EditorGizmoOperation::Translate)) {
        services.editorSession.gizmoOperation = EditorGizmoOperation::Translate;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (R)", services.editorSession.gizmoOperation == EditorGizmoOperation::Rotate)) {
        services.editorSession.gizmoOperation = EditorGizmoOperation::Rotate;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale (S)", services.editorSession.gizmoOperation == EditorGizmoOperation::Scale)) {
        services.editorSession.gizmoOperation = EditorGizmoOperation::Scale;
    }
    ImGui::SameLine();
    int space = static_cast<int>(services.editorSession.gizmoSpace);
    if (services.editorSession.gizmoOperation == EditorGizmoOperation::Scale) {
        space = static_cast<int>(EditorGizmoSpace::Local);
    }
    ImGui::SetNextItemWidth(78.0f);
    if (ImGui::Combo("##Space", &space, "Local\0World\0")) {
        services.editorSession.gizmoSpace = static_cast<EditorGizmoSpace>(space);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &services.editorSession.gizmoSnapEnabled);
    if (!hasTransform) ImGui::EndDisabled();
    if (worldRotationBlocked) {
        ImGui::TextColored(
            ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
            "World rotation is disabled under a non-uniformly scaled parent."
        );
    }
    ImGui::End();
}

}  // namespace

void drawEditorGizmo(EngineServices& services) {
    const std::optional<SceneObjectId> selectedObject = selectedAuthoredObject(services);
    const std::optional<EntityId> selectedEntity = selectedObject.has_value()
        ? services.sceneDocument.entityForObject(*selectedObject)
        : std::nullopt;
    TransformComponent* transform = selectedEntity.has_value()
        ? services.world.transforms.tryGet(*selectedEntity)
        : nullptr;

    std::optional<glm::mat4> parentWorld{};
    if (selectedEntity.has_value()) {
        if (const ParentComponent* parent = services.world.parents.tryGet(*selectedEntity);
            parent != nullptr && services.world.isAlive(parent->parent)) {
            parentWorld = editorEntityWorldMatrix(services.world, parent->parent);
        }
    }
    const bool worldRotationBlocked = transform != nullptr && parentWorld.has_value() &&
        services.editorSession.gizmoOperation == EditorGizmoOperation::Rotate &&
        services.editorSession.gizmoSpace == EditorGizmoSpace::World &&
        editorParentHasNonUniformScale(*parentWorld);
    drawGizmoToolbar(services, transform != nullptr, worldRotationBlocked);

    if (transform == nullptr || !selectedObject.has_value() || !selectedEntity.has_value() ||
        worldRotationBlocked) {
        finishGizmoCommand(services);
        return;
    }
    if (services.editorSession.gizmoWasUsing &&
        services.editorSession.gizmoActiveObjectId != *selectedObject) {
        finishGizmoCommand(services);
    }

    const TransformComponent beforeFrame = *transform;
    glm::mat4 worldMatrix = editorEntityWorldMatrix(services.world, *selectedEntity);
    const render::CameraMatrices camera = computeCameraMatrices(
        services.camera,
        services.renderer.width(),
        services.renderer.height()
    );
    ImGuizmo::SetOrthographic(!services.camera.freeCameraEnabled);
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    ImGuizmo::SetRect(
        0.0f,
        0.0f,
        static_cast<float>(services.renderer.width()),
        static_cast<float>(services.renderer.height())
    );
    ImGuizmo::PushID(static_cast<int>(selectedEntity->index));

    const ImGuizmo::OPERATION operation = imGuizmoOperation(services.editorSession.gizmoOperation);
    const ImGuizmo::MODE mode = services.editorSession.gizmoOperation == EditorGizmoOperation::Scale ||
        services.editorSession.gizmoSpace == EditorGizmoSpace::Local
        ? ImGuizmo::LOCAL
        : ImGuizmo::WORLD;
    float snap[3]{0.25f, 0.25f, 0.25f};
    if (services.editorSession.gizmoOperation == EditorGizmoOperation::Rotate) {
        snap[0] = snap[1] = snap[2] = 15.0f;
    } else if (services.editorSession.gizmoOperation == EditorGizmoOperation::Scale) {
        snap[0] = snap[1] = snap[2] = 0.1f;
    }
    const bool changed = ImGuizmo::Manipulate(
        glm::value_ptr(camera.view),
        glm::value_ptr(camera.projection),
        operation,
        mode,
        glm::value_ptr(worldMatrix),
        nullptr,
        services.editorSession.gizmoSnapEnabled ? snap : nullptr
    );
    ImGuizmo::PopID();
    const bool usingGizmo = ImGuizmo::IsUsing();
    if (usingGizmo && !services.editorSession.gizmoWasUsing) {
        services.editorSession.gizmoStartTransform = beforeFrame;
        services.editorSession.gizmoActiveObjectId = *selectedObject;
    }
    if (changed) {
        TransformComponent local{};
        if (editorLocalTransformFromWorld(worldMatrix, parentWorld, local)) {
            applyAuthoredTransform(services, *selectedObject, local);
        }
    }
    if (!usingGizmo && services.editorSession.gizmoWasUsing) {
        finishGizmoCommand(services);
    } else {
        services.editorSession.gizmoWasUsing = usingGizmo;
    }
}

}  // namespace core
