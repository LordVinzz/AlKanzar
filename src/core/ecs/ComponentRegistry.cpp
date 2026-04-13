#include "ComponentRegistry.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include "World.hpp"
#include "core/app/EngineServices.hpp"
#include "core/editor/Command.hpp"

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
        editSnapshotCommand<render::Material>(
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

    editSnapshotCommand<render::Material>(
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

    editSnapshotCommand<render::Material>(
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

    editSnapshotCommand<render::Material>(
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

    editSnapshotCommand<render::Material>(
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

}  // namespace

ComponentRegistry::ComponentRegistry() {
    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Transform,
        "Transform",
        "Core",
        [](const World& world, EntityId entity) {
            return world.transforms.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.transforms.emplace(entity, TransformComponent{});
        },
        [](World& world, EntityId entity) {
            world.transforms.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            TransformComponent* transform = services.world.transforms.tryGet(entity);
            if (transform == nullptr) {
                return false;
            }
            bool changed = false;
            editSnapshotCommand<TransformComponent>(
                "Position",
                "Move Entity",
                "transform-position-" + std::to_string(entity.index),
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
                "Rotate Entity",
                "transform-rotation-" + std::to_string(entity.index),
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
                "Scale Entity",
                "transform-scale-" + std::to_string(entity.index),
                *transform,
                [&services, entity](const TransformComponent& snapshot) {
                    if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                        *target = snapshot;
                        target->scale = glm::max(target->scale, glm::vec3(0.01f));
                        notifyTransformChanged(services, entity);
                    }
                },
                [](TransformComponent& edited) {
                    const bool localChanged = ImGui::DragFloat3("Scale", glm::value_ptr(edited.scale), 0.02f);
                    if (localChanged) {
                        edited.scale = glm::max(edited.scale, glm::vec3(0.01f));
                    }
                    return localChanged;
                },
                services.commands
            );
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Visibility,
        "Visibility",
        "Rendering",
        [](const World& world, EntityId entity) {
            return world.visibilities.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.visibilities.emplace(entity, VisibilityComponent{true});
        },
        [](World& world, EntityId entity) {
            world.visibilities.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            VisibilityComponent* vis = services.world.visibilities.tryGet(entity);
            if (vis == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::Checkbox("Visible", &vis->visible);
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Renderable,
        "Renderable",
        "Rendering",
        [](const World& world, EntityId entity) {
            return world.renderables.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.renderables.emplace(entity, RenderableComponent{});
        },
        [](World& world, EntityId entity) {
            world.renderables.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            RenderableComponent* renderable = services.world.renderables.tryGet(entity);
            if (renderable == nullptr) {
                return false;
            }
            bool changed = false;
            ImGui::Text("Mesh Handle: %zu", renderable->mesh.value);
            int layer = static_cast<int>(renderable->layer);
            if (ImGui::Combo("Render Layer", &layer, "Ground\0Geometry\0Actors\0")) {
                renderable->layer = static_cast<render::RenderLayer>(layer);
                changed = true;
            }
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Material,
        "Material",
        "Rendering",
        [](const World& world, EntityId entity) {
            return world.materials.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.materials.emplace(entity, MaterialComponent{std::make_shared<render::Material>()});
        },
        [](World& world, EntityId entity) {
            world.materials.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            drawMaterialInspector(services, entity);
            return true;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::LightVolume,
        "Light Volume",
        "Lighting",
        [](const World& world, EntityId entity) {
            return world.lightVolumes.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.lightVolumes.emplace(entity, LightVolumeComponent{});
        },
        [](World& world, EntityId entity) {
            world.lightVolumes.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            LightVolumeComponent* volume = services.world.lightVolumes.tryGet(entity);
            if (volume == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat3("Half Extents", glm::value_ptr(volume->halfExtents), 0.05f, 0.01f, 1000.0f);
            if (changed) {
                volume->halfExtents = glm::max(volume->halfExtents, glm::vec3(0.01f));
            }
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::PointLight,
        "Point Light",
        "Lighting",
        [](const World& world, EntityId entity) {
            return world.pointLights.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.pointLights.emplace(entity, PointLightComponent{});
        },
        [](World& world, EntityId entity) {
            world.pointLights.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            PointLightComponent* light = services.world.pointLights.tryGet(entity);
            if (light == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat("Radius", &light->radius, 0.1f, 0.1f, 128.0f);
            changed |= ImGui::ColorEdit3("Color", glm::value_ptr(light->color));
            changed |= ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 128.0f);
            changed |= ImGui::Checkbox("Movable", &light->isMovable);
            changed |= ImGui::Checkbox("Casts Shadow", &light->castsShadow);
            if (changed) {
                notifyLightChanged(services, entity);
            }
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::SpotLight,
        "Spot Light",
        "Lighting",
        [](const World& world, EntityId entity) {
            return world.spotLights.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.spotLights.emplace(entity, SpotLightComponent{});
        },
        [](World& world, EntityId entity) {
            world.spotLights.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            SpotLightComponent* light = services.world.spotLights.tryGet(entity);
            if (light == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat("Radius", &light->radius, 0.1f, 0.1f, 128.0f);
            changed |= ImGui::ColorEdit3("Color", glm::value_ptr(light->color));
            changed |= ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 128.0f);
            changed |= ImGui::DragFloat3("Target", glm::value_ptr(light->target), 0.05f);
            changed |= ImGui::DragFloat("Inner Angle", &light->innerAngle, 0.25f, 1.0f, light->outerAngle);
            changed |= ImGui::DragFloat("Outer Angle", &light->outerAngle, 0.25f, light->innerAngle, 80.0f);
            changed |= ImGui::Checkbox("Movable", &light->isMovable);
            changed |= ImGui::Checkbox("Casts Shadow", &light->castsShadow);
            if (changed) {
                notifyLightChanged(services, entity);
            }
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::BoxCollider,
        "Box Collider",
        "Physics",
        [](const World& world, EntityId entity) {
            return world.boxColliders.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.boxColliders.emplace(entity, BoxColliderComponent{});
        },
        [](World& world, EntityId entity) {
            world.boxColliders.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            BoxColliderComponent* collider = services.world.boxColliders.tryGet(entity);
            if (collider == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat3("Center", glm::value_ptr(collider->center), 0.01f);
            changed |= ImGui::DragFloat3("Half Extents", glm::value_ptr(collider->halfExtents), 0.01f, 0.001f, 1000.0f);
            changed |= ImGui::Checkbox("Is Trigger", &collider->isTrigger);
            changed |= ImGui::Checkbox("Show Collider", &collider->showDebug);
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::SphereCollider,
        "Sphere Collider",
        "Physics",
        [](const World& world, EntityId entity) {
            return world.sphereColliders.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.sphereColliders.emplace(entity, SphereColliderComponent{});
        },
        [](World& world, EntityId entity) {
            world.sphereColliders.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            SphereColliderComponent* collider = services.world.sphereColliders.tryGet(entity);
            if (collider == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat3("Center", glm::value_ptr(collider->center), 0.01f);
            changed |= ImGui::DragFloat("Radius", &collider->radius, 0.01f, 0.001f, 1000.0f);
            changed |= ImGui::Checkbox("Is Trigger", &collider->isTrigger);
            changed |= ImGui::Checkbox("Show Collider", &collider->showDebug);
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Rigidbody,
        "Rigidbody",
        "Physics",
        [](const World& world, EntityId entity) {
            return world.rigidbodies.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.rigidbodies.emplace(entity, RigidbodyComponent{});
        },
        [](World& world, EntityId entity) {
            world.rigidbodies.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            RigidbodyComponent* rb = services.world.rigidbodies.tryGet(entity);
            if (rb == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat("Mass", &rb->mass, 0.1f, 0.001f, 10000.0f);
            changed |= ImGui::DragFloat("Linear Damping", &rb->linearDamping, 0.01f, 0.0f, 1.0f);
            changed |= ImGui::DragFloat("Angular Damping", &rb->angularDamping, 0.01f, 0.0f, 1.0f);
            changed |= ImGui::Checkbox("Is Kinematic", &rb->isKinematic);
            changed |= ImGui::Checkbox("Use Gravity", &rb->useGravity);
            ImGui::Separator();
            ImGui::Text("Velocity: %.2f %.2f %.2f", rb->velocity.x, rb->velocity.y, rb->velocity.z);
            ImGui::Text("Angular Vel: %.2f %.2f %.2f", rb->angularVelocity.x, rb->angularVelocity.y, rb->angularVelocity.z);
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::NavAgent,
        "Nav Agent",
        "Navigation",
        [](const World& world, EntityId entity) {
            return world.navAgents.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.navAgents.emplace(entity, NavAgentComponent{});
        },
        [](World& world, EntityId entity) {
            world.navAgents.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            NavAgentComponent* agent = services.world.navAgents.tryGet(entity);
            if (agent == nullptr) {
                return false;
            }
            bool changed = false;
            const bool ownsSphere = services.world.sphereColliders.contains(entity);
            const bool ownsBox = services.world.boxColliders.contains(entity);
            changed |= ImGui::DragFloat("Move Speed", &agent->moveSpeed, 0.05f, 0.0f, 50.0f);
            changed |= ImGui::DragFloat("Turn Speed (deg/s)", &agent->turnSpeedDeg, 1.0f, 0.0f, 1080.0f);
            changed |= ImGui::DragFloat("Arrival Radius", &agent->arrivalRadius, 0.01f, 0.01f, 10.0f);
            int clearanceSource = static_cast<int>(agent->clearanceSource);
            const char* clearanceSources[] = {"None", "Auto", "Sphere", "Box"};
            if (ImGui::Combo("Clearance Collider", &clearanceSource, clearanceSources, 4)) {
                agent->clearanceSource = static_cast<NavAgentClearanceSource>(clearanceSource);
                changed = true;
            }
            ImGui::Text("Owned Colliders: sphere %s, box %s", ownsSphere ? "yes" : "no", ownsBox ? "yes" : "no");
            ImGui::Separator();
            ImGui::Text("Moving: %s", agent->moving ? "yes" : "no");
            if (agent->destination.has_value()) {
                const glm::vec3& dest = *agent->destination;
                ImGui::Text("Destination: %.2f %.2f %.2f", dest.x, dest.y, dest.z);
            } else {
                ImGui::TextUnformatted("Destination: none");
            }
            ImGui::Text("Path Corners: %d", static_cast<int>(agent->pathCorners.size()));
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Locomotion,
        "Locomotion",
        "Navigation",
        [](const World& world, EntityId entity) {
            return world.locomotion.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.locomotion.emplace(entity, LocomotionComponent{});
        },
        [](World& world, EntityId entity) {
            world.locomotion.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            LocomotionComponent* loco = services.world.locomotion.tryGet(entity);
            if (loco == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::InputInt("Idle Clip", &loco->idleClip);
            changed |= ImGui::InputInt("Walk Clip", &loco->walkClip);
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::NavSource,
        "Nav Source",
        "Navigation",
        [](const World& world, EntityId entity) {
            return world.navSources.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.navSources.emplace(entity, NavSourceComponent{});
        },
        [](World& world, EntityId entity) {
            world.navSources.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            NavSourceComponent* nav = services.world.navSources.tryGet(entity);
            if (nav == nullptr) {
                return false;
            }
            bool changed = false;
            const char* tagNames[] = {"Walkable", "Blocking", "Ignored"};
            int defaultTag = static_cast<int>(nav->defaultTag);
            if (ImGui::Combo("Default Tag", &defaultTag, tagNames, 3)) {
                nav->defaultTag = static_cast<NavSourceTag>(defaultTag);
                changed = true;
            }
            int effectiveTag = static_cast<int>(nav->effectiveTag);
            if (ImGui::Combo("Effective Tag", &effectiveTag, tagNames, 3)) {
                nav->effectiveTag = static_cast<NavSourceTag>(effectiveTag);
                changed = true;
            }
            return changed;
        },
    });
}

const ComponentDescriptor* ComponentRegistry::find(ComponentKind kind) const {
    const auto it = std::find_if(descriptors_.begin(), descriptors_.end(), [&](const ComponentDescriptor& descriptor) {
        return descriptor.kind == kind;
    });
    return it != descriptors_.end() ? &*it : nullptr;
}

void ComponentRegistry::drawAddComponentButton(EngineServices& services, EntityId entity) const {
    if (ImGui::Button("Add Component...")) {
        ImGui::OpenPopup("AddComponentPopup");
    }

    if (ImGui::BeginPopup("AddComponentPopup")) {
        std::string currentCategory;
        for (const ComponentDescriptor& descriptor : descriptors_) {
            if (descriptor.hasComponent(services.world, entity)) {
                continue;
            }

            if (descriptor.category != currentCategory) {
                if (!currentCategory.empty()) {
                    ImGui::Separator();
                }
                ImGui::TextDisabled("%s", descriptor.category.c_str());
                currentCategory = descriptor.category;
            }

            if (ImGui::Selectable(descriptor.name.c_str())) {
                descriptor.addComponent(services.world, entity);
                if (descriptor.kind == ComponentKind::PointLight ||
                    descriptor.kind == ComponentKind::SpotLight ||
                    descriptor.kind == ComponentKind::LightVolume) {
                    notifyLightChanged(services, entity);
                }
                if (descriptor.kind == ComponentKind::Transform) {
                    notifyTransformChanged(services, entity);
                }
                if (descriptor.kind == ComponentKind::Material) {
                    notifyMaterialChanged(services, entity);
                }
            }
        }

        ImGui::EndPopup();
    }
}

void ComponentRegistry::drawComponentTabs(
    EngineServices& services,
    EntityId entity,
    std::optional<ComponentKind> focusedComponent
) const {
    for (const ComponentDescriptor& descriptor : descriptors_) {
        if (!descriptor.hasComponent(services.world, entity)) {
            continue;
        }

        ImGui::PushID(descriptor.name.c_str());

        bool open = true;
        const ImGuiTabItemFlags tabFlags =
            focusedComponent.has_value() && *focusedComponent == descriptor.kind
                ? ImGuiTabItemFlags_SetSelected
                : 0;
        if (ImGui::BeginTabItem(descriptor.name.c_str(), &open, tabFlags)) {
            ImGui::BeginChild(("##comp_" + descriptor.name).c_str(), ImVec2(0.0f, 0.0f), false);
            descriptor.drawInspector(services, entity);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (!open) {
            descriptor.removeComponent(services.world, entity);
            if (descriptor.kind == ComponentKind::PointLight ||
                descriptor.kind == ComponentKind::SpotLight ||
                descriptor.kind == ComponentKind::LightVolume) {
                notifyLightChanged(services, entity);
            }
            if (descriptor.kind == ComponentKind::Transform) {
                notifyTransformChanged(services, entity);
            }
            if (descriptor.kind == ComponentKind::Material) {
                notifyMaterialChanged(services, entity);
            }
        }

        ImGui::PopID();
    }
}

}  // namespace core
