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


void drawTextureSlotEditor(
    EngineServices& services,
    const std::string& materialName,
    EntityId entity,
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
            [&services, entity](const render::Material& snapshot) {
                if (MaterialComponent* materialComponent = services.world.materials.tryGet(entity);
                    materialComponent != nullptr && materialComponent->material) {
                    *materialComponent->material = snapshot;
                    notifyEditorMaterialChanged(services, entity);
                }
            },
            false
        ));
    }

    if (ref->bindingMode == render::TextureBindingMode::ProjectTexture) {
        const bool isTextureBrowserTarget = services.editorSession.textureBrowserSlot == slot;
        const char* textureTargetLabel =
            isTextureBrowserTarget ? "Pick texture in Texture Browser" : "Set texture as target";
        if (ImGui::Button(textureTargetLabel)) {
            services.editorSession.textureBrowserSlot = slot;
            services.editorSession.activeInspectorTab = InspectorTab::TextureBrowser;
            services.editorSession.textureBrowserFocusRequested = true;
        }
    } else if (ref->bindingMode == render::TextureBindingMode::InlineValue) {
        editEditorSnapshot<render::Material>(
            "InlineValue",
            "Edit Inline Material Value",
            "material-inline-" + std::to_string(entity.index) + "-" + std::to_string(static_cast<int>(slot)),
            material,
            [&services, entity](const render::Material& snapshot) {
                if (MaterialComponent* materialComponent = services.world.materials.tryGet(entity);
                    materialComponent != nullptr && materialComponent->material) {
                    *materialComponent->material = snapshot;
                    render::TextureRef* target =
                        render::textureRefForSlot(*materialComponent->material, services.editorSession.textureBrowserSlot);
                    (void)target;
                    notifyEditorMaterialChanged(services, entity);
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
            [&services, entity](const render::Material& snapshot) {
                if (MaterialComponent* materialComponent = services.world.materials.tryGet(entity);
                    materialComponent != nullptr && materialComponent->material) {
                    *materialComponent->material = snapshot;
                    notifyEditorMaterialChanged(services, entity);
                }
            },
            false
        ));
    }

    ImGui::TreePop();
    ImGui::PopID();
}

void drawMaterialTextureSlotGrid(
    EngineServices& services,
    const std::string& materialName,
    EntityId entity,
    render::Material& material
) {
    static constexpr render::MaterialTextureSlot kTextureSlots[] = {
        render::MaterialTextureSlot::BaseColor,
        render::MaterialTextureSlot::MetallicRoughness,
        render::MaterialTextureSlot::Normal,
        render::MaterialTextureSlot::Ao,
        render::MaterialTextureSlot::Emissive,
        render::MaterialTextureSlot::Alpha,
        render::MaterialTextureSlot::Clearcoat,
        render::MaterialTextureSlot::DetailNormal,
        render::MaterialTextureSlot::Height,
    };

    ImGui::SeparatorText("Texture Slots");

    constexpr float kSlotColumnMinWidth = 280.0f;
    const float availableWidth = std::max(ImGui::GetContentRegionAvail().x, kSlotColumnMinWidth);
    const int columnCount = std::clamp(static_cast<int>(availableWidth / kSlotColumnMinWidth), 1, 2);
    if (!ImGui::BeginTable(
            "MaterialTextureSlots",
            columnCount,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings)) {
        return;
    }

    for (render::MaterialTextureSlot slot : kTextureSlots) {
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(slot));

        constexpr float kSlotCardPadding = 8.0f;
        constexpr float kSlotCardRounding = 6.0f;

        const ImVec2 cellStartScreen = ImGui::GetCursorScreenPos();
        const ImVec2 cellStart = ImGui::GetCursorPos();
        const float cellWidth = ImGui::GetContentRegionAvail().x;

        ImGui::SetCursorPos(ImVec2(cellStart.x + kSlotCardPadding, cellStart.y + kSlotCardPadding));
        ImGui::BeginGroup();
        drawTextureSlotEditor(services, materialName, entity, slot, material);
        ImGui::EndGroup();

        const ImVec2 groupMax = ImGui::GetItemRectMax();
        ImGui::Dummy(ImVec2(0.0f, kSlotCardPadding));

        const ImVec2 cardMin = cellStartScreen;
        const ImVec2 cardMax(cellStartScreen.x + cellWidth, groupMax.y + kSlotCardPadding);
        ImGui::GetWindowDrawList()->AddRect(
            cardMin,
            cardMax,
            ImGui::GetColorU32(ImVec4(0.48f, 0.48f, 0.48f, 0.9f)),
            kSlotCardRounding,
            0,
            1.0f
        );

        ImGui::PopID();
    }

    ImGui::EndTable();
}

void drawInspectorWindow(EngineServices& services) {
    if (!services.editorSession.inspectorWindowVisible) {
        services.editorSession.inspectorWindowFocusRequested = false;
        services.editorSession.textureBrowserFocusRequested = false;
        return;
    }

    if (services.editorSession.inspectorWindowFocusRequested) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::SetNextWindowSize(ImVec2(640.0f, 720.0f), ImGuiCond_FirstUseEver);

    std::string inspectorTitle = "Inspector";
    if (services.selection.current().has_value()) {
        inspectorTitle += " - " + editorSelectionLabel(services);
    }
    inspectorTitle += "###Inspector";

    const bool wasOpen = services.editorSession.inspectorWindowVisible;
    bool open = wasOpen;
    if (!ImGui::Begin(inspectorTitle.c_str(), &open)) {
        ImGui::End();
        services.editorSession.inspectorWindowVisible = open;
        if (wasOpen != open) {
            markEditorSessionImGuiSettingsDirty();
        }
        services.editorSession.inspectorWindowFocusRequested = false;
        services.editorSession.textureBrowserFocusRequested = false;
        return;
    }

    if (!services.selection.current().has_value()) {
        ImGui::TextUnformatted("Click an object or light to inspect it.");
        ImGui::End();
        services.editorSession.inspectorWindowVisible = open;
        if (wasOpen != open) {
            markEditorSessionImGuiSettingsDirty();
        }
        services.editorSession.inspectorWindowFocusRequested = false;
        services.editorSession.textureBrowserFocusRequested = false;
        return;
    }

    const SelectionTarget selectedTarget = *services.selection.current();
    const EntityId selected = selectedTarget.entity;
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
        if (ImGui::BeginTabItem("Entity", nullptr, selectionTabFlags)) {
            services.editorSession.activeInspectorTab = InspectorTab::Selection;
            ImGui::BeginChild("SelectionInspectorContent", ImVec2(0.0f, 0.0f), false);

            const std::size_t childCount = editorHierarchyChildCount(services, selected);
            ImGui::Text("Entity: %s", editorEntityLabel(services, selected).c_str());
            ImGui::Text("Id: %u", selected.index);
            if (selectedTarget.component.has_value()) {
                if (const ComponentDescriptor* descriptor = services.componentRegistry.find(*selectedTarget.component)) {
                    ImGui::Text("Focused Component: %s", descriptor->name.c_str());
                }
            }
            ImGui::Text("Components: %d", static_cast<int>(std::count_if(
                services.componentRegistry.descriptors().begin(),
                services.componentRegistry.descriptors().end(),
                [&](const ComponentDescriptor& descriptor) {
                    return descriptor.hasComponent(services.world, selected);
                }
            )));
            if (childCount > 0u) {
                ImGui::Text("Child Nodes: %d", static_cast<int>(childCount));
            }
            if (selected.index < services.world.transformCache_.size() &&
                services.world.transformCache_[selected.index].hasWorldBounds) {
                const render::Bounds3& bounds = services.world.transformCache_[selected.index].worldBounds;
                ImGui::Separator();
                ImGui::Text("World Bounds Min: %.2f %.2f %.2f", bounds.min.x, bounds.min.y, bounds.min.z);
                ImGui::Text("World Bounds Max: %.2f %.2f %.2f", bounds.max.x, bounds.max.y, bounds.max.z);
            }

            drawAnimationInspector(services, selected);

            ImGui::Separator();
            ImGui::Spacing();
            services.componentRegistry.drawAddComponentButton(services, selected);

            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        services.componentRegistry.drawComponentTabs(services, selected, selectedTarget.component);

        if (services.world.materials.contains(selected) &&
            ImGui::BeginTabItem("Texture Browser", nullptr, browserTabFlags)) {
            if (MaterialComponent* material = services.world.materials.tryGet(selected);
                material != nullptr && material->material) {
                drawTextureBrowser(services, selected, *material->material);
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
    services.editorSession.inspectorWindowVisible = open;
    if (wasOpen != open) {
        markEditorSessionImGuiSettingsDirty();
    }
    services.editorSession.inspectorWindowFocusRequested = false;
    services.editorSession.textureBrowserFocusRequested = false;
}

}  // namespace core
