#include "ComponentRegistry.hpp"
#include "ComponentInspector.hpp"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "core/ecs/World.hpp"
#include "core/app/EngineServices.hpp"

namespace core {
namespace {

bool isLightComponent(ComponentKind kind) {
    return kind == ComponentKind::DirectionalLight ||
        kind == ComponentKind::PointLight ||
        kind == ComponentKind::SpotLight ||
        kind == ComponentKind::LightVolume;
}

bool capturesSceneDataOnPresenceChange(ComponentKind kind) {
    return kind == ComponentKind::Character ||
        kind == ComponentKind::Combatant;
}

}  // namespace

const ComponentDescriptor* ComponentRegistry::find(ComponentKind kind) const {
    const auto it = std::find_if(descriptors_.begin(), descriptors_.end(), [&](const ComponentDescriptor& descriptor) {
        return descriptor.kind == kind;
    });
    return it != descriptors_.end() ? &*it : nullptr;
}

void ComponentRegistry::drawAddComponentButton(EngineServices& services, EntityId entity) const {
    if (!services.sceneDocument.isAuthoredEntity(entity)) {
        ImGui::TextDisabled("Runtime-generated entities are read-only.");
        return;
    }
    ImGui::TextDisabled("Added technical components are runtime-only unless represented by SCN V2.");
    if (ImGui::Button("Add Component...")) {
        ImGui::OpenPopup("AddComponentPopup");
    }

    if (ImGui::BeginPopup("AddComponentPopup")) {
        std::string currentCategory;
        for (const ComponentDescriptor& descriptor : descriptors_) {
            if (descriptor.hasComponent(services.world, entity)) {
                continue;
            }
            if (descriptor.canAddComponent &&
                !descriptor.canAddComponent(services.world, entity)) {
                continue;
            }
            if (descriptor.kind == ComponentKind::DirectionalLight &&
                !services.world.directionalLights.entities().empty()) {
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
                if (capturesSceneDataOnPresenceChange(descriptor.kind)) {
                    if (const std::optional<SceneObjectId> object =
                            services.sceneDocument.objectForEntity(entity)) {
                        (void)services.sceneDocument.captureRuntimeObject(
                            *object,
                            services.world
                        );
                    }
                }
                if (isLightComponent(descriptor.kind)) {
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
        const bool authored = services.sceneDocument.isAuthoredEntity(entity);
        const ImGuiTabItemFlags tabFlags =
            focusedComponent.has_value() && *focusedComponent == descriptor.kind
                ? ImGuiTabItemFlags_SetSelected
                : 0;
        if (ImGui::BeginTabItem(descriptor.name.c_str(), authored ? &open : nullptr, tabFlags)) {
            ImGui::BeginChild(("##comp_" + descriptor.name).c_str(), ImVec2(0.0f, 0.0f), false);
            if (!authored) {
                ImGui::BeginDisabled();
            }
            descriptor.drawInspector(services, entity);
            if (!authored) {
                ImGui::EndDisabled();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (authored && !open) {
            descriptor.removeComponent(services.world, entity);
            if (capturesSceneDataOnPresenceChange(descriptor.kind)) {
                if (const std::optional<SceneObjectId> object =
                        services.sceneDocument.objectForEntity(entity)) {
                    (void)services.sceneDocument.captureRuntimeObject(
                        *object,
                        services.world
                    );
                }
            }
            if (isLightComponent(descriptor.kind)) {
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
