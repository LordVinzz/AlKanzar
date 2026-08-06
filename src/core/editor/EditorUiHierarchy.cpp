#include "core/editor/EditorUi.hpp"
#include "core/editor/EditorUiCommands.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#include "core/app/EngineServices.hpp"
#include "core/editor/EditorSessionImGuiSettings.hpp"
#include "core/editor/ComponentRegistry.hpp"
#include "render/resources/StaticGltfModel.hpp"


namespace core {


struct SceneHierarchyData {
    std::vector<EntityId> roots{};
    std::vector<std::vector<EntityId>> childrenByParent{};
};

std::size_t sceneHierarchyChildCount(const SceneHierarchyData& hierarchy, EntityId entity) {
    if (entity.index >= hierarchy.childrenByParent.size()) {
        return 0u;
    }
    return hierarchy.childrenByParent[entity.index].size();
}

SceneHierarchyData buildSceneHierarchy(const EngineServices& services) {
    SceneHierarchyData hierarchy{};
    const World& world = services.world;

    std::vector<EntityId> entities{};
    std::vector<bool> seen(std::max(world.transformCache_.size(), world.lightRuntime_.size()), false);

    auto ensureIndex = [&](std::size_t index) {
        if (seen.size() <= index) {
            seen.resize(index + 1u, false);
        }
        if (hierarchy.childrenByParent.size() <= index) {
            hierarchy.childrenByParent.resize(index + 1u);
        }
    };

    auto addEntity = [&](EntityId entity) {
        if (!entity.valid() || !world.isAlive(entity)) {
            return;
        }
        ensureIndex(entity.index);
        if (seen[entity.index]) {
            return;
        }
        seen[entity.index] = true;
        entities.push_back(entity);
    };

    auto collectEntities = [&](const auto& store) {
        for (EntityId entity : store.entities()) {
            addEntity(entity);
        }
    };

    collectEntities(world.names);
    collectEntities(world.transforms);
    collectEntities(world.visibilities);
    collectEntities(world.renderables);
    collectEntities(world.materials);
    collectEntities(world.lightVolumes);
    collectEntities(world.boxColliders);
    collectEntities(world.sphereColliders);
    collectEntities(world.rigidbodies);
    collectEntities(world.navAgents);
    collectEntities(world.locomotion);
    collectEntities(world.navSources);
    collectEntities(world.animatedModels);
    collectEntities(world.skinnedRenderables);
    collectEntities(world.pointLights);
    collectEntities(world.spotLights);
    collectEntities(world.characters);
    collectEntities(world.abilityScores);
    collectEntities(world.skillRanks);
    collectEntities(world.characterVitals);

    for (EntityId child : world.parents.entities()) {
        addEntity(child);
        const ParentComponent& parent = world.parents.get(child);
        addEntity(parent.parent);
    }

    const auto compareEntitiesByLabel = [&](EntityId lhs, EntityId rhs) {
        const std::string lhsLabel = editorEntityLabel(services, lhs);
        const std::string rhsLabel = editorEntityLabel(services, rhs);
        if (lhsLabel != rhsLabel) {
            return lhsLabel < rhsLabel;
        }
        return lhs.index < rhs.index;
    };

    std::sort(entities.begin(), entities.end(), compareEntitiesByLabel);

    for (EntityId entity : entities) {
        const ParentComponent* parent = world.parents.tryGet(entity);
        if (parent == nullptr || !parent->parent.valid() || !world.isAlive(parent->parent)) {
            continue;
        }

        ensureIndex(parent->parent.index);
        hierarchy.childrenByParent[parent->parent.index].push_back(entity);
    }

    for (auto& children : hierarchy.childrenByParent) {
        std::sort(children.begin(), children.end(), compareEntitiesByLabel);
    }

    for (EntityId entity : entities) {
        const ParentComponent* parent = world.parents.tryGet(entity);
        if (parent == nullptr ||
            !parent->parent.valid() ||
            !world.isAlive(parent->parent) ||
            parent->parent.index >= seen.size() ||
            !seen[parent->parent.index]) {
            hierarchy.roots.push_back(entity);
        }
    }

    return hierarchy;
}

void selectHierarchyEntity(EngineServices& services, EntityId entity) {
    services.editorSelection.set(entity);
    services.editorSession.activeInspectorTab = InspectorTab::Selection;
    services.editorSession.textureBrowserFocusRequested = true;
}

void selectHierarchyComponent(EngineServices& services, EntityId entity, ComponentKind component) {
    services.editorSelection.set(SelectionTarget{entity, component});
    services.editorSession.activeInspectorTab = InspectorTab::Selection;
    services.editorSession.textureBrowserFocusRequested = true;
}

void drawTransformControls(
    EngineServices& services,
    EntityId entity,
    const std::string& moveLabel,
    const std::string& rotateLabel,
    const std::string& scaleLabel,
    const std::string& mergeKeyPrefix
) {
    if (TransformComponent* transform = services.world.transforms.tryGet(entity)) {
        editEditorSnapshot<TransformComponent>(
            "Position",
            moveLabel,
            mergeKeyPrefix + "-position-" + std::to_string(entity.index),
            *transform,
            [&services, entity](const TransformComponent& snapshot) {
                if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                    *target = snapshot;
                    notifyEditorTransformChanged(services, entity);
                }
            },
            [](TransformComponent& edited) {
                return ImGui::DragFloat3("Position", glm::value_ptr(edited.position), 0.05f);
            },
            services.commands
        );
        editEditorSnapshot<TransformComponent>(
            "Rotation",
            rotateLabel,
            mergeKeyPrefix + "-rotation-" + std::to_string(entity.index),
            *transform,
            [&services, entity](const TransformComponent& snapshot) {
                if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                    *target = snapshot;
                    notifyEditorTransformChanged(services, entity);
                }
            },
            [](TransformComponent& edited) {
                return ImGui::DragFloat3("Rotation", glm::value_ptr(edited.rotationDeg), 0.5f);
            },
            services.commands
        );
        editEditorSnapshot<TransformComponent>(
            "Scale",
            scaleLabel,
            mergeKeyPrefix + "-scale-" + std::to_string(entity.index),
            *transform,
            [&services, entity](const TransformComponent& snapshot) {
                if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                    *target = snapshot;
                    target->scale = glm::max(target->scale, glm::vec3(0.01f));
                    notifyEditorTransformChanged(services, entity);
                }
            },
            [](TransformComponent& edited) {
                const bool changed = ImGui::DragFloat3("Scale", glm::value_ptr(edited.scale), 0.02f);
                if (changed) {
                    edited.scale = glm::max(edited.scale, glm::vec3(0.01f));
                }
                return changed;
            },
            services.commands
        );
    }
}

void drawVisibilityControl(EngineServices& services, EntityId entity) {
    if (!services.world.visibilities.contains(entity)) {
        return;
    }

    bool visible = services.world.visibilities.get(entity).visible;
    if (ImGui::Checkbox("Visible", &visible)) {
        const bool before = services.world.visibilities.get(entity).visible;
        services.commands.execute(std::make_unique<SnapshotCommand<bool>>(
            "Toggle Visibility",
            std::string{},
            before,
            visible,
            [&services, entity](const bool& snapshot) {
                if (VisibilityComponent* target = services.world.visibilities.tryGet(entity)) {
                    target->visible = snapshot;
                    notifyEditorTransformChanged(services, entity);
                }
            },
            false
        ));
    }
}

void drawSceneHierarchyNode(EngineServices& services, const SceneHierarchyData& hierarchy, EntityId entity) {
    static const std::vector<EntityId> emptyChildren{};

    const std::vector<EntityId>& children = entity.index < hierarchy.childrenByParent.size()
        ? hierarchy.childrenByParent[entity.index]
        : emptyChildren;
    const std::optional<SelectionTarget>& selection = services.editorSelection.current();
    const bool entitySelected = selection.has_value() &&
        selection->entity == entity &&
        !selection->component.has_value();
    int componentCount = 0;
    for (const ComponentDescriptor& descriptor : services.componentRegistry.descriptors()) {
        if (descriptor.hasComponent(services.world, entity)) {
            componentCount++;
        }
    }

    ImGui::PushID(static_cast<int>(entity.index));
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (entitySelected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (children.empty() && componentCount == 0) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    } else {
        flags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    }

    const bool open = ImGui::TreeNodeEx("node", flags, "%s", editorHierarchyLabel(services, entity).c_str());
    if (ImGui::IsItemClicked()) {
        selectHierarchyEntity(services, entity);
    }

    if (open && (componentCount > 0 || !children.empty())) {
        for (const ComponentDescriptor& descriptor : services.componentRegistry.descriptors()) {
            if (!descriptor.hasComponent(services.world, entity)) {
                continue;
            }

            ImGui::PushID(static_cast<int>(descriptor.kind));
            ImGuiTreeNodeFlags componentFlags =
                ImGuiTreeNodeFlags_Leaf |
                ImGuiTreeNodeFlags_NoTreePushOnOpen |
                ImGuiTreeNodeFlags_SpanAvailWidth;
            if (selection.has_value() &&
                selection->entity == entity &&
                selection->component.has_value() &&
                *selection->component == descriptor.kind) {
                componentFlags |= ImGuiTreeNodeFlags_Selected;
            }

            ImGui::TreeNodeEx("component", componentFlags, "%s", descriptor.name.c_str());
            if (ImGui::IsItemClicked()) {
                selectHierarchyComponent(services, entity, descriptor.kind);
            }
            ImGui::PopID();
        }
        for (EntityId child : children) {
            drawSceneHierarchyNode(services, hierarchy, child);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void drawSceneHierarchyWindow(EngineServices& services) {
    if (!services.editorSession.sceneHierarchyVisible) {
        services.editorSession.sceneHierarchyFocusRequested = false;
        return;
    }

    if (services.editorSession.sceneHierarchyFocusRequested) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowSize(ImVec2(360.0f, 420.0f), ImGuiCond_FirstUseEver);

    const bool wasOpen = services.editorSession.sceneHierarchyVisible;
    bool open = wasOpen;
    if (ImGui::Begin("Scene Hierarchy", &open)) {
        const SceneHierarchyData hierarchy = buildSceneHierarchy(services);
        if (hierarchy.roots.empty()) {
            ImGui::TextUnformatted("No scene entities available.");
        } else if (ImGui::TreeNodeEx(
                       "CurrentScene",
                       ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                       "Current Scene")) {
            for (EntityId root : hierarchy.roots) {
                drawSceneHierarchyNode(services, hierarchy, root);
            }
            ImGui::TreePop();
        }
    }
    ImGui::End();

    services.editorSession.sceneHierarchyVisible = open;
    if (wasOpen != open) {
        markEditorSessionImGuiSettingsDirty();
    }
    services.editorSession.sceneHierarchyFocusRequested = false;
}
std::size_t editorHierarchyChildCount(const EngineServices& services, EntityId entity) {
    const SceneHierarchyData hierarchy = buildSceneHierarchy(services);
    return sceneHierarchyChildCount(hierarchy, entity);
}

}  // namespace core
