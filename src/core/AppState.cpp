#include "AppState.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#include "Command.hpp"
#include "EngineServices.hpp"

namespace core {

namespace {

template <typename TSnapshot, typename ApplyFn, typename DrawFn>
void editSnapshotCommand(
    const char* itemId,
    const std::string& label,
    const std::string& mergeKey,
    TSnapshot currentValue,
    ApplyFn applyFn,
    DrawFn drawFn,
    CommandHistory& history,
    bool mergeable = true
) {
    static std::unordered_map<ImGuiID, TSnapshot> snapshotById;

    const TSnapshot before = currentValue;
    TSnapshot edited = currentValue;
    const bool changed = drawFn(edited);
    const ImGuiID imguiId = ImGui::GetItemID();

    if (ImGui::IsItemActivated()) {
        snapshotById[imguiId] = before;
    }

    if (changed) {
        applyFn(edited);
    }

    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const auto it = snapshotById.find(imguiId);
        const TSnapshot committedBefore = it != snapshotById.end() ? it->second : before;
        history.execute(std::make_unique<SnapshotCommand<TSnapshot>>(
            label,
            mergeKey,
            committedBefore,
            edited,
            applyFn,
            mergeable
        ));
        snapshotById.erase(imguiId);
    } else if (!ImGui::IsItemActive()) {
        snapshotById.erase(imguiId);
    }
}

std::string entityLabel(const EngineServices& services, EntityId entity) {
    if (const NameComponent* name = services.world.names.tryGet(entity)) {
        return name->value;
    }
    return "Entity " + std::to_string(entity.index);
}

std::string selectionLabel(const EngineServices& services) {
    if (!services.selection.current().has_value()) {
        return "None";
    }

    return entityLabel(services, *services.selection.current());
}

std::string hierarchyLabel(const EngineServices& services, EntityId entity) {
    std::string label = entityLabel(services, entity);
    if (services.world.pointLights.contains(entity)) {
        label += " [Point Light]";
    } else if (services.world.spotLights.contains(entity)) {
        label += " [Spot Light]";
    } else if (services.world.renderables.contains(entity)) {
        label += " [Mesh]";
    } else if (services.world.transforms.contains(entity)) {
        label += " [Group]";
    } else {
        label += " [Node]";
    }
    return label;
}

void notifyTransformChanged(EngineServices& services, EntityId entity) {
    services.world.markTransformsDirty(entity);
    services.events.publish(TransformChangedEvent{entity});
}

void notifyLightChanged(EngineServices& services, EntityId entity) {
    services.world.markLightsDirty(entity);
    services.events.publish(LightChangedEvent{entity});
}

void notifyMaterialChanged(EngineServices& services, MaterialHandle materialHandle) {
    services.materials.notifyChanged(materialHandle);
    services.events.publish(MaterialChangedEvent{materialHandle});
}

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
    collectEntities(world.pointLights);
    collectEntities(world.spotLights);

    for (EntityId child : world.parents.entities()) {
        addEntity(child);
        const ParentComponent& parent = world.parents.get(child);
        addEntity(parent.parent);
    }

    const auto compareEntitiesByLabel = [&](EntityId lhs, EntityId rhs) {
        const std::string lhsLabel = entityLabel(services, lhs);
        const std::string rhsLabel = entityLabel(services, rhs);
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
    services.selection.set(entity);
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
        editSnapshotCommand<TransformComponent>(
            "Position",
            moveLabel,
            mergeKeyPrefix + "-position-" + std::to_string(entity.index),
            *transform,
            [&services, entity](const TransformComponent& snapshot) {
                if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                    *target = snapshot;
                    notifyTransformChanged(services, entity);
                }
            },
            [](TransformComponent& edited) {
                return ImGui::DragFloat3("Position", glm::value_ptr(edited.position), 0.05f);
            },
            services.commands
        );
        editSnapshotCommand<TransformComponent>(
            "Rotation",
            rotateLabel,
            mergeKeyPrefix + "-rotation-" + std::to_string(entity.index),
            *transform,
            [&services, entity](const TransformComponent& snapshot) {
                if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                    *target = snapshot;
                    notifyTransformChanged(services, entity);
                }
            },
            [](TransformComponent& edited) {
                return ImGui::DragFloat3("Rotation", glm::value_ptr(edited.rotationDeg), 0.5f);
            },
            services.commands
        );
        editSnapshotCommand<TransformComponent>(
            "Scale",
            scaleLabel,
            mergeKeyPrefix + "-scale-" + std::to_string(entity.index),
            *transform,
            [&services, entity](const TransformComponent& snapshot) {
                if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                    *target = snapshot;
                    target->scale = glm::max(target->scale, glm::vec3(0.01f));
                    notifyTransformChanged(services, entity);
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
                    notifyTransformChanged(services, entity);
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
    const bool isSelected = services.selection.current().has_value() && *services.selection.current() == entity;

    ImGui::PushID(static_cast<int>(entity.index));
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (isSelected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    } else {
        flags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    }

    const bool open = ImGui::TreeNodeEx("node", flags, "%s", hierarchyLabel(services, entity).c_str());
    if (ImGui::IsItemClicked()) {
        selectHierarchyEntity(services, entity);
    }

    if (open && !children.empty()) {
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

    bool open = services.editorSession.sceneHierarchyVisible;
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
    services.editorSession.sceneHierarchyFocusRequested = false;
}

void drawTextureBrowser(EngineServices& services, render::Material& material, MaterialHandle materialHandle) {
    render::TextureRef* targetRef = render::textureRefForSlot(material, services.editorSession.textureBrowserSlot);
    if (targetRef == nullptr) {
        ImGui::TextUnformatted("No texture slot selected.");
        return;
    }

    const render::TextureSemantic semantic = render::textureSemanticForSlot(services.editorSession.textureBrowserSlot);
    std::vector<std::shared_ptr<render::Texture>> filtered = services.renderer.textureCatalog(semantic);
    if (services.editorSession.textureBrowserSearch[0] != '\0') {
        std::string needle(services.editorSession.textureBrowserSearch.data());
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        filtered.erase(
            std::remove_if(
                filtered.begin(),
                filtered.end(),
                [&needle](const std::shared_ptr<render::Texture>& texture) {
                    std::string lowered = texture->name;
                    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char value) {
                        return static_cast<char>(std::tolower(value));
                    });
                    return lowered.find(needle) == std::string::npos;
                }
            ),
            filtered.end()
        );
    }

    ImGui::Text("Target Material: %s", material.name.c_str());
    ImGui::Text("Target Slot: %s", render::materialTextureSlotName(services.editorSession.textureBrowserSlot));
    if (ImGui::Button("Back To Selection")) {
        services.editorSession.activeInspectorTab = InspectorTab::Selection;
        services.editorSession.textureBrowserFocusRequested = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
        "##TextureSearch",
        "Search textures...",
        services.editorSession.textureBrowserSearch.data(),
        static_cast<int>(services.editorSession.textureBrowserSearch.size())
    );
    ImGui::Separator();
    ImGui::Text("%d textures", static_cast<int>(filtered.size()));

    ImGui::BeginChild("TextureBrowserResults", ImVec2(0.0f, 420.0f), false);
    const float cellWidth = 180.0f;
    const float availableWidth = std::max(ImGui::GetContentRegionAvail().x, cellWidth);
    const int columnCount = std::max(1, static_cast<int>(availableWidth / cellWidth));
    if (ImGui::BeginTable("TextureBrowserGrid", columnCount, ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& texture : filtered) {
            ImGui::TableNextColumn();
            ImGui::PushID(texture.get());

            if (void* previewId = services.renderer.texturePreviewId(texture)) {
                ImGui::Image(previewId, ImVec2(96.0f, 96.0f), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
            } else {
                ImGui::BeginChild("NoPreview", ImVec2(96.0f, 96.0f), true);
                ImGui::TextUnformatted("No");
                ImGui::TextUnformatted("Preview");
                ImGui::EndChild();
            }

            ImGui::TextWrapped("%s", texture->name.c_str());
            ImGui::Text("%s | %s", render::textureOriginName(texture->origin), render::textureSemanticName(texture->semantic));
            ImGui::Text("%dx%d | %s", texture->width, texture->height, render::formatName(texture->format));
            if (ImGui::Button("Use Texture", ImVec2(-1.0f, 0.0f))) {
                const render::Material before = material;
                targetRef->texture = texture;
                targetRef->sampler = services.renderer.defaultSampler();
                targetRef->bindingMode = render::TextureBindingMode::ProjectTexture;
                const render::Material after = material;
                services.commands.execute(std::make_unique<SnapshotCommand<render::Material>>(
                    "Assign Texture",
                    std::string{},
                    before,
                    after,
                    [&services, materialHandle](const render::Material& snapshot) {
                        if (std::shared_ptr<render::Material> materialPtr = services.materials.get(materialHandle)) {
                            *materialPtr = snapshot;
                            notifyMaterialChanged(services, materialHandle);
                        }
                    },
                    false
                ));
                services.editorSession.activeInspectorTab = InspectorTab::Selection;
                services.editorSession.textureBrowserFocusRequested = true;
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void drawTextureSlotEditor(
    EngineServices& services,
    const std::string& materialName,
    MaterialHandle materialHandle,
    render::MaterialTextureSlot slot,
    render::Material& material
) {
    render::TextureRef* ref = render::textureRefForSlot(material, slot);
    if (ref == nullptr) {
        return;
    }

    ImGui::PushID(static_cast<int>(slot));
    if (!ImGui::TreeNodeEx("slot", ImGuiTreeNodeFlags_DefaultOpen, "%s", render::materialTextureSlotName(slot))) {
        ImGui::PopID();
        return;
    }

    if (ref->texture) {
        if (void* previewId = services.renderer.texturePreviewId(ref->texture)) {
            ImGui::Image(previewId, ImVec2(64.0f, 64.0f), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        }
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("Mode: %s", render::textureBindingModeName(ref->bindingMode));
    ImGui::Text("Texture: %s", ref->texture ? ref->texture->name.c_str() : "Default");
    ImGui::EndGroup();

    const render::Material beforeMaterial = material;
    int bindingMode = static_cast<int>(ref->bindingMode);
    if (ImGui::Combo("Binding Mode", &bindingMode, "Default\0Project Texture\0Inline Value\0")) {
        ref->bindingMode = static_cast<render::TextureBindingMode>(bindingMode);
        if (ref->bindingMode == render::TextureBindingMode::ProjectTexture) {
            auto catalog = services.renderer.textureCatalog(render::textureSemanticForSlot(slot));
            ref->texture = catalog.empty() ? nullptr : catalog.front();
            ref->sampler = services.renderer.defaultSampler();
        } else if (ref->bindingMode == render::TextureBindingMode::InlineValue) {
            ref->inlineValue = render::defaultInlineValueForSlot(slot);
            ref->texture = render::makeSolidTexture(
                materialName + " / " + render::materialTextureSlotName(slot) + " Inline",
                ref->inlineValue,
                false,
                render::textureSemanticForSlot(slot),
                render::TextureOrigin::InlinePrivate
            );
            ref->sampler = services.renderer.defaultSampler();
            services.renderer.registerTexture(ref->texture);
        }

        services.commands.execute(std::make_unique<SnapshotCommand<render::Material>>(
            "Texture Binding",
            std::string{},
            beforeMaterial,
            material,
            [&services, materialHandle](const render::Material& snapshot) {
                if (std::shared_ptr<render::Material> materialPtr = services.materials.get(materialHandle)) {
                    *materialPtr = snapshot;
                    notifyMaterialChanged(services, materialHandle);
                }
            },
            false
        ));
    }

    if (ref->bindingMode == render::TextureBindingMode::ProjectTexture) {
        if (ImGui::Button("Browse Texture Library")) {
            services.editorSession.textureBrowserSlot = slot;
            services.editorSession.activeInspectorTab = InspectorTab::TextureBrowser;
            services.editorSession.textureBrowserFocusRequested = true;
        }
    } else if (ref->bindingMode == render::TextureBindingMode::InlineValue) {
        editSnapshotCommand<render::Material>(
            "InlineValue",
            "Edit Inline Material Value",
            "material-inline-" + std::to_string(materialHandle.value) + "-" + std::to_string(static_cast<int>(slot)),
            material,
            [&services, materialHandle](const render::Material& snapshot) {
                if (std::shared_ptr<render::Material> materialPtr = services.materials.get(materialHandle)) {
                    *materialPtr = snapshot;
                    render::TextureRef* target = render::textureRefForSlot(*materialPtr, services.editorSession.textureBrowserSlot);
                    (void)target;
                    notifyMaterialChanged(services, materialHandle);
                }
            },
            [&material, ref, slot](render::Material& editedMaterial) {
                render::TextureRef* editedRef = render::textureRefForSlot(editedMaterial, slot);
                bool changed = false;
                switch (slot) {
                    case render::MaterialTextureSlot::BaseColor:
                        changed = ImGui::ColorEdit4("Inline Value", glm::value_ptr(editedRef->inlineValue));
                        break;
                    case render::MaterialTextureSlot::MetallicRoughness: {
                        glm::vec3 value(editedRef->inlineValue);
                        changed = ImGui::DragFloat3("Inline AO/Rough/Metal", glm::value_ptr(value), 0.01f, 0.0f, 1.0f, "%.2f");
                        if (changed) {
                            editedRef->inlineValue = glm::vec4(value, 1.0f);
                        }
                        break;
                    }
                    case render::MaterialTextureSlot::Normal:
                    case render::MaterialTextureSlot::DetailNormal: {
                        glm::vec3 value(editedRef->inlineValue);
                        changed = ImGui::DragFloat3("Inline Normal", glm::value_ptr(value), 0.01f, 0.0f, 1.0f, "%.2f");
                        if (changed) {
                            editedRef->inlineValue = glm::vec4(value, 1.0f);
                        }
                        break;
                    }
                    case render::MaterialTextureSlot::Ao:
                    case render::MaterialTextureSlot::Alpha:
                    case render::MaterialTextureSlot::Height: {
                        float scalar = editedRef->inlineValue.x;
                        changed = ImGui::DragFloat("Inline Value", &scalar, 0.01f, 0.0f, 1.0f, "%.2f");
                        if (changed) {
                            editedRef->inlineValue = glm::vec4(scalar, scalar, scalar, 1.0f);
                        }
                        break;
                    }
                    case render::MaterialTextureSlot::Emissive: {
                        glm::vec3 value(editedRef->inlineValue);
                        changed = ImGui::ColorEdit3("Inline Value", glm::value_ptr(value));
                        if (changed) {
                            editedRef->inlineValue = glm::vec4(value, 1.0f);
                        }
                        break;
                    }
                    case render::MaterialTextureSlot::Clearcoat: {
                        glm::vec2 value(editedRef->inlineValue.x, editedRef->inlineValue.y);
                        changed = ImGui::DragFloat2("Inline Factor/Rough", glm::value_ptr(value), 0.01f, 0.0f, 1.0f, "%.2f");
                        if (changed) {
                            editedRef->inlineValue = glm::vec4(value, 0.0f, 1.0f);
                        }
                        break;
                    }
                }

                if (changed) {
                    editedRef->texture = render::makeSolidTexture(
                        material.name + " / " + render::materialTextureSlotName(slot) + " Inline",
                        editedRef->inlineValue,
                        false,
                        render::textureSemanticForSlot(slot),
                        render::TextureOrigin::InlinePrivate
                    );
                }
                return changed;
            },
            services.commands,
            true
        );
    }

    int uvSet = std::clamp(ref->uvSet, 0, 1);
    if (ImGui::SliderInt("UV Set", &uvSet, 0, 1)) {
        const render::Material before = material;
        ref->uvSet = uvSet;
        services.commands.execute(std::make_unique<SnapshotCommand<render::Material>>(
            "Change UV Set",
            std::string{},
            before,
            material,
            [&services, materialHandle](const render::Material& snapshot) {
                if (std::shared_ptr<render::Material> materialPtr = services.materials.get(materialHandle)) {
                    *materialPtr = snapshot;
                    notifyMaterialChanged(services, materialHandle);
                }
            },
            false
        ));
    }

    ImGui::TreePop();
    ImGui::PopID();
}

}  // namespace

void BootstrapState::onEnter(EngineServices& services) {
    if (!services.renderer.init()) {
        services.requestedMode = AppMode::Shutdown;
        return;
    }
    services.sceneLoaded = services.sceneFactory.buildScene(
        services.sceneRegistry.defaultScene(),
        services.world,
        services.materials,
        services.renderer
    );
    if (!services.sceneLoaded) {
        spdlog::error("Application: failed to build default scene");
        services.requestedMode = AppMode::Shutdown;
        return;
    }
    services.requestedMode = AppMode::Gameplay;
}

void BootstrapState::onExit(EngineServices&) {}
void BootstrapState::update(EngineServices&) {}
void BootstrapState::renderUi(EngineServices&) {}

void GameplayState::onEnter(EngineServices&) {}

void GameplayState::onExit(EngineServices&) {}

void GameplayState::update(EngineServices& services) {
    updateOrbitCamera(services.camera, services.time);
}

void GameplayState::renderUi(EngineServices&) {}

void EditorState::onEnter(EngineServices&) {}

void EditorState::onExit(EngineServices&) {}

void EditorState::update(EngineServices& services) {
    updateOrbitCamera(services.camera, services.time);
}

void EditorState::renderUi(EngineServices& services) {
    drawSceneHierarchyWindow(services);
    if (!services.editorSession.sceneHierarchyVisible) {
        services.editorSession.textureBrowserFocusRequested = false;
        return;
    }

    ImGui::SetNextWindowBgAlpha(0.92f);
    std::string inspectorTitle = "Inspector";
    if (services.selection.current().has_value()) {
        inspectorTitle += " - " + selectionLabel(services);
    }
    inspectorTitle += "###Inspector";
    ImGui::Begin(inspectorTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    if (!services.selection.current().has_value()) {
        ImGui::TextUnformatted("Click an object or light to inspect it.");
        ImGui::End();
        return;
    }

    const EntityId selected = *services.selection.current();
    const ImGuiTabItemFlags selectionTabFlags =
        (services.editorSession.textureBrowserFocusRequested &&
         services.editorSession.activeInspectorTab == InspectorTab::Selection)
            ? ImGuiTabItemFlags_SetSelected
            : 0;
    const ImGuiTabItemFlags browserTabFlags =
        (services.editorSession.textureBrowserFocusRequested &&
         services.editorSession.activeInspectorTab == InspectorTab::TextureBrowser)
            ? ImGuiTabItemFlags_SetSelected
            : 0;

    if (ImGui::BeginTabBar("InspectorTabs")) {
        if (ImGui::BeginTabItem("Selection", nullptr, selectionTabFlags)) {
            services.editorSession.activeInspectorTab = InspectorTab::Selection;

            if (services.world.renderables.contains(selected)) {
                const EntityId transformEntity = services.world.editableTransformEntity(selected);
                drawTransformControls(
                    services,
                    transformEntity,
                    "Move Entity",
                    "Rotate Entity",
                    "Scale Entity",
                    "transform"
                );
                drawVisibilityControl(services, selected);

                if (selected.index < services.world.transformCache_.size() &&
                    services.world.transformCache_[selected.index].hasWorldBounds) {
                    const render::Bounds3& bounds = services.world.transformCache_[selected.index].worldBounds;
                    ImGui::Separator();
                    ImGui::Text("World Bounds Min: %.2f %.2f %.2f", bounds.min.x, bounds.min.y, bounds.min.z);
                    ImGui::Text("World Bounds Max: %.2f %.2f %.2f", bounds.max.x, bounds.max.y, bounds.max.z);
                }

                const RenderableComponent& renderable = services.world.renderables.get(selected);
                if (std::shared_ptr<render::Material> material = services.materials.get(renderable.material)) {
                    ImGui::Separator();
                    ImGui::Text("Material Asset: %s", material->name.c_str());

                    editSnapshotCommand<render::Material>(
                        "BaseColorFactor",
                        "Edit Base Color",
                        "material-base-color-" + std::to_string(renderable.material.value),
                        *material,
                        [&services, handle = renderable.material](const render::Material& snapshot) {
                            if (std::shared_ptr<render::Material> target = services.materials.get(handle)) {
                                *target = snapshot;
                                notifyMaterialChanged(services, handle);
                            }
                        },
                        [](render::Material& edited) {
                            return ImGui::ColorEdit3("Base Color Factor", glm::value_ptr(edited.baseColorFactor));
                        },
                        services.commands
                    );
                    editSnapshotCommand<render::Material>(
                        "MetallicFactor",
                        "Edit Metallic",
                        "material-metallic-" + std::to_string(renderable.material.value),
                        *material,
                        [&services, handle = renderable.material](const render::Material& snapshot) {
                            if (std::shared_ptr<render::Material> target = services.materials.get(handle)) {
                                *target = snapshot;
                                notifyMaterialChanged(services, handle);
                            }
                        },
                        [](render::Material& edited) {
                            return ImGui::DragFloat("Metallic Factor", &edited.metallicFactor, 0.01f, 0.0f, 1.0f);
                        },
                        services.commands
                    );
                    editSnapshotCommand<render::Material>(
                        "RoughnessFactor",
                        "Edit Roughness",
                        "material-roughness-" + std::to_string(renderable.material.value),
                        *material,
                        [&services, handle = renderable.material](const render::Material& snapshot) {
                            if (std::shared_ptr<render::Material> target = services.materials.get(handle)) {
                                *target = snapshot;
                                notifyMaterialChanged(services, handle);
                            }
                        },
                        [](render::Material& edited) {
                            return ImGui::DragFloat("Roughness Factor", &edited.roughnessFactor, 0.01f, 0.0f, 1.0f);
                        },
                        services.commands
                    );
                    editSnapshotCommand<render::Material>(
                        "NormalScale",
                        "Edit Normal Scale",
                        "material-normal-scale-" + std::to_string(renderable.material.value),
                        *material,
                        [&services, handle = renderable.material](const render::Material& snapshot) {
                            if (std::shared_ptr<render::Material> target = services.materials.get(handle)) {
                                *target = snapshot;
                                notifyMaterialChanged(services, handle);
                            }
                        },
                        [](render::Material& edited) {
                            return ImGui::DragFloat("Normal Scale", &edited.normalScale, 0.01f, 0.0f, 8.0f);
                        },
                        services.commands
                    );

                    drawTextureSlotEditor(services, material->name, renderable.material, render::MaterialTextureSlot::BaseColor, *material);
                    drawTextureSlotEditor(services, material->name, renderable.material, render::MaterialTextureSlot::MetallicRoughness, *material);
                    drawTextureSlotEditor(services, material->name, renderable.material, render::MaterialTextureSlot::Normal, *material);
                    drawTextureSlotEditor(services, material->name, renderable.material, render::MaterialTextureSlot::Ao, *material);
                    drawTextureSlotEditor(services, material->name, renderable.material, render::MaterialTextureSlot::Emissive, *material);
                    drawTextureSlotEditor(services, material->name, renderable.material, render::MaterialTextureSlot::Alpha, *material);
                    drawTextureSlotEditor(services, material->name, renderable.material, render::MaterialTextureSlot::Clearcoat, *material);
                    drawTextureSlotEditor(services, material->name, renderable.material, render::MaterialTextureSlot::DetailNormal, *material);
                    drawTextureSlotEditor(services, material->name, renderable.material, render::MaterialTextureSlot::Height, *material);
                }
            } else if (PointLightComponent* pointLight = services.world.pointLights.tryGet(selected)) {
                if (TransformComponent* transform = services.world.transforms.tryGet(selected)) {
                    editSnapshotCommand<TransformComponent>(
                        "Position",
                        "Move Point Light",
                        "point-light-position-" + std::to_string(selected.index),
                        *transform,
                        [&services, selected](const TransformComponent& snapshot) {
                            if (TransformComponent* target = services.world.transforms.tryGet(selected)) {
                                *target = snapshot;
                                notifyTransformChanged(services, selected);
                                notifyLightChanged(services, selected);
                            }
                        },
                        [](TransformComponent& edited) {
                            return ImGui::DragFloat3("Position", glm::value_ptr(edited.position), 0.05f);
                        },
                        services.commands
                    );
                }

                editSnapshotCommand<PointLightComponent>(
                    "PointLightProps",
                    "Edit Point Light",
                    "point-light-props-" + std::to_string(selected.index),
                    *pointLight,
                    [&services, selected](const PointLightComponent& snapshot) {
                        if (PointLightComponent* target = services.world.pointLights.tryGet(selected)) {
                            *target = snapshot;
                            notifyLightChanged(services, selected);
                        }
                    },
                    [](PointLightComponent& edited) {
                        bool changed = false;
                        changed |= ImGui::DragFloat("Radius", &edited.radius, 0.1f, 0.1f, 128.0f);
                        changed |= ImGui::ColorEdit3("Color", glm::value_ptr(edited.color));
                        changed |= ImGui::DragFloat("Intensity", &edited.intensity, 0.1f, 0.0f, 128.0f);
                        changed |= ImGui::Checkbox("Movable", &edited.isMovable);
                        changed |= ImGui::Checkbox("Casts Shadow", &edited.castsShadow);
                        return changed;
                    },
                    services.commands
                );
            } else if (SpotLightComponent* spotLight = services.world.spotLights.tryGet(selected)) {
                if (TransformComponent* transform = services.world.transforms.tryGet(selected)) {
                    editSnapshotCommand<TransformComponent>(
                        "Position",
                        "Move Spot Light",
                        "spot-light-position-" + std::to_string(selected.index),
                        *transform,
                        [&services, selected](const TransformComponent& snapshot) {
                            if (TransformComponent* target = services.world.transforms.tryGet(selected)) {
                                *target = snapshot;
                                notifyTransformChanged(services, selected);
                                notifyLightChanged(services, selected);
                            }
                        },
                        [](TransformComponent& edited) {
                            return ImGui::DragFloat3("Position", glm::value_ptr(edited.position), 0.05f);
                        },
                        services.commands
                    );
                }

                editSnapshotCommand<SpotLightComponent>(
                    "SpotLightProps",
                    "Edit Spot Light",
                    "spot-light-props-" + std::to_string(selected.index),
                    *spotLight,
                    [&services, selected](const SpotLightComponent& snapshot) {
                        if (SpotLightComponent* target = services.world.spotLights.tryGet(selected)) {
                            *target = snapshot;
                            notifyLightChanged(services, selected);
                        }
                    },
                    [](SpotLightComponent& edited) {
                        bool changed = false;
                        changed |= ImGui::DragFloat("Radius", &edited.radius, 0.1f, 0.1f, 128.0f);
                        changed |= ImGui::ColorEdit3("Color", glm::value_ptr(edited.color));
                        changed |= ImGui::DragFloat("Intensity", &edited.intensity, 0.1f, 0.0f, 128.0f);
                        changed |= ImGui::DragFloat3("Target", glm::value_ptr(edited.target), 0.05f);
                        changed |= ImGui::DragFloat("Inner Angle", &edited.innerAngle, 0.25f, 1.0f, edited.outerAngle);
                        changed |= ImGui::DragFloat("Outer Angle", &edited.outerAngle, 0.25f, edited.innerAngle, 80.0f);
                        changed |= ImGui::Checkbox("Movable", &edited.isMovable);
                        changed |= ImGui::Checkbox("Casts Shadow", &edited.castsShadow);
                        return changed;
                    },
                    services.commands
                );
            } else {
                const EntityId transformEntity = services.world.editableTransformEntity(selected);
                if (transformEntity.valid()) {
                    drawTransformControls(
                        services,
                        transformEntity,
                        "Move Entity",
                        "Rotate Entity",
                        "Scale Entity",
                        "scene-node"
                    );
                }

                drawVisibilityControl(services, selected);

                const SceneHierarchyData hierarchy = buildSceneHierarchy(services);
                const std::size_t childCount = sceneHierarchyChildCount(hierarchy, selected);
                if (selected.index < services.world.transformCache_.size() &&
                    services.world.transformCache_[selected.index].hasWorldBounds) {
                    const render::Bounds3& bounds = services.world.transformCache_[selected.index].worldBounds;
                    ImGui::Separator();
                    ImGui::Text("World Bounds Min: %.2f %.2f %.2f", bounds.min.x, bounds.min.y, bounds.min.z);
                    ImGui::Text("World Bounds Max: %.2f %.2f %.2f", bounds.max.x, bounds.max.y, bounds.max.z);
                }
                if (childCount > 0u) {
                    ImGui::Separator();
                    ImGui::Text("Child Nodes: %d", static_cast<int>(childCount));
                } else if (!transformEntity.valid()) {
                    ImGui::TextUnformatted("Selected scene node has no editable properties.");
                }
            }

            ImGui::EndTabItem();
        }

        if (services.world.renderables.contains(selected) &&
            ImGui::BeginTabItem("Texture Browser", nullptr, browserTabFlags)) {
            if (std::shared_ptr<render::Material> material = services.materials.get(
                    services.world.renderables.get(selected).material)) {
                drawTextureBrowser(services, *material, services.world.renderables.get(selected).material);
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    services.editorSession.textureBrowserFocusRequested = false;
    ImGui::End();
}

void ShutdownState::onEnter(EngineServices& services) {
    services.running = false;
}

void ShutdownState::onExit(EngineServices&) {}
void ShutdownState::update(EngineServices&) {}
void ShutdownState::renderUi(EngineServices&) {}

}  // namespace core
