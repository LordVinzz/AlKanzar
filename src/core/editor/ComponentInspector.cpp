#include "ComponentInspector.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include <glm/gtc/type_ptr.hpp>

#include "core/ecs/World.hpp"
#include "core/app/EngineServices.hpp"

namespace core {

std::string entityLabel(const EngineServices& services, EntityId entity) {
    if (const NameComponent* name = services.world.names.tryGet(entity)) {
        return name->value;
    }
    return "Entity " + std::to_string(entity.index);
}

void notifyTransformChanged(EngineServices& services, EntityId entity) {
    services.world.markTransformsDirty(entity);
    services.events.publish(TransformChangedEvent{entity});
    if (services.world.pointLights.contains(entity) || services.world.spotLights.contains(entity)) {
        services.world.markLightsDirty(entity);
        services.events.publish(LightChangedEvent{entity});
    }
}

void notifyLightChanged(EngineServices& services, EntityId entity) {
    services.world.markLightsDirty(entity);
    services.events.publish(LightChangedEvent{entity});
}

void notifyMaterialChanged(EngineServices& services, EntityId entity) {
    services.events.publish(MaterialChangedEvent{entity});
}

render::Material& ensureMaterial(EngineServices& services, EntityId entity) {
    MaterialComponent* materialComponent = services.world.materials.tryGet(entity);
    if (materialComponent == nullptr) {
        materialComponent = &services.world.materials.emplace(entity, MaterialComponent{});
    }
    if (!materialComponent->material) {
        materialComponent->material = std::make_shared<render::Material>();
        materialComponent->material->name = entityLabel(services, entity) + " Material";
    } else if (materialComponent->material->name.empty()) {
        materialComponent->material->name = entityLabel(services, entity) + " Material";
    }
    return *materialComponent->material;
}

void applyMaterialSnapshot(EngineServices& services, EntityId entity, const render::Material& snapshot) {
    render::Material& material = ensureMaterial(services, entity);
    material = snapshot;
    notifyMaterialChanged(services, entity);
}

void drawMaterialTextureSlotEditor(
    EngineServices& services,
    EntityId entity,
    const std::string& materialName,
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
                applyMaterialSnapshot(services, entity, snapshot);
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
        editComponentSnapshot<render::Material>(
            "InlineValue",
            "Edit Inline Material Value",
            "material-inline-" + std::to_string(entity.index) + "-" + std::to_string(static_cast<int>(slot)),
            material,
            [&services, entity](const render::Material& snapshot) {
                applyMaterialSnapshot(services, entity, snapshot);
            },
            [slot](render::Material& editedMaterial) {
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
                        editedMaterial.name + " / " + render::materialTextureSlotName(slot) + " Inline",
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
                applyMaterialSnapshot(services, entity, snapshot);
            },
            false
        ));
    }

    ImGui::TreePop();
    ImGui::PopID();
}

void drawMaterialTextureSlotGrid(EngineServices& services, EntityId entity, render::Material& material) {
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
        drawMaterialTextureSlotEditor(services, entity, material.name, slot, material);
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

void drawMaterialInspector(EngineServices& services, EntityId entity) {
    render::Material& material = ensureMaterial(services, entity);
    ImGui::Text("Material: %s", material.name.c_str());

    editComponentSnapshot<render::Material>(
        "BaseColorFactor",
        "Edit Base Color",
        "material-base-color-" + std::to_string(entity.index),
        material,
        [&services, entity](const render::Material& snapshot) {
            applyMaterialSnapshot(services, entity, snapshot);
        },
        [](render::Material& edited) {
            return ImGui::ColorEdit3("Base Color Factor", glm::value_ptr(edited.baseColorFactor));
        },
        services.commands
    );

    editComponentSnapshot<render::Material>(
        "MetallicFactor",
        "Edit Metallic",
        "material-metallic-" + std::to_string(entity.index),
        material,
        [&services, entity](const render::Material& snapshot) {
            applyMaterialSnapshot(services, entity, snapshot);
        },
        [](render::Material& edited) {
            return ImGui::DragFloat("Metallic Factor", &edited.metallicFactor, 0.01f, 0.0f, 1.0f);
        },
        services.commands
    );

    editComponentSnapshot<render::Material>(
        "RoughnessFactor",
        "Edit Roughness",
        "material-roughness-" + std::to_string(entity.index),
        material,
        [&services, entity](const render::Material& snapshot) {
            applyMaterialSnapshot(services, entity, snapshot);
        },
        [](render::Material& edited) {
            return ImGui::DragFloat("Roughness Factor", &edited.roughnessFactor, 0.01f, 0.0f, 1.0f);
        },
        services.commands
    );

    editComponentSnapshot<render::Material>(
        "NormalScale",
        "Edit Normal Scale",
        "material-normal-scale-" + std::to_string(entity.index),
        material,
        [&services, entity](const render::Material& snapshot) {
            applyMaterialSnapshot(services, entity, snapshot);
        },
        [](render::Material& edited) {
            return ImGui::DragFloat("Normal Scale", &edited.normalScale, 0.01f, 0.0f, 8.0f);
        },
        services.commands
    );

    drawMaterialTextureSlotGrid(services, entity, material);
}

}  // namespace core
