#include "ComponentRegistry.hpp"
#include "ComponentInspector.hpp"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "World.hpp"
#include "core/app/EngineServices.hpp"

namespace core {

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

