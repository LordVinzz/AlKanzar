#include "ComponentRegistry.hpp"

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include "ComponentInspector.hpp"
#include "core/app/EngineServices.hpp"
#include "core/ecs/World.hpp"

namespace core {
namespace {

template <typename Component, typename DrawFn, typename SanitizeFn>
void editLightField(
    EngineServices& services,
    EntityId entity,
    const char* itemId,
    const char* label,
    Component current,
    DrawFn draw,
    SanitizeFn sanitize
) {
    editComponentSnapshot<Component>(
        itemId,
        label,
        "scene-light-" + std::to_string(entity.index) + "-" + itemId,
        current,
        [&services, entity, sanitize](const Component& snapshot) {
            Component* target = nullptr;
            if constexpr (std::is_same_v<Component, LightVolumeComponent>) {
                target = services.world.lightVolumes.tryGet(entity);
            } else if constexpr (std::is_same_v<Component, PointLightComponent>) {
                target = services.world.pointLights.tryGet(entity);
            } else {
                target = services.world.spotLights.tryGet(entity);
            }
            if (target != nullptr) {
                *target = snapshot;
                sanitize(*target);
                notifyLightChanged(services, entity);
            }
        },
        std::move(draw),
        services.commands
    );
}

void sanitizeVolume(LightVolumeComponent& volume) {
    volume.halfExtents = glm::max(volume.halfExtents, glm::vec3(0.01f));
}

void sanitizePoint(PointLightComponent& light) {
    light.radius = std::max(light.radius, 0.01f);
    light.color = glm::max(light.color, glm::vec3(0.0f));
    light.intensity = std::max(light.intensity, 0.0f);
    light.shadowBiasMin = std::max(light.shadowBiasMin, 0.0f);
    light.shadowBiasSlope = std::max(light.shadowBiasSlope, 0.0f);
}

void sanitizeSpot(SpotLightComponent& light) {
    light.radius = std::max(light.radius, 0.01f);
    light.color = glm::max(light.color, glm::vec3(0.0f));
    light.intensity = std::max(light.intensity, 0.0f);
    light.innerAngle = std::clamp(light.innerAngle, 0.0f, 178.0f);
    light.outerAngle = std::clamp(light.outerAngle, light.innerAngle, 179.0f);
    light.shadowBiasMin = std::max(light.shadowBiasMin, 0.0f);
    light.shadowBiasSlope = std::max(light.shadowBiasSlope, 0.0f);
}

bool drawLightVolume(EngineServices& services, EntityId entity) {
    LightVolumeComponent* volume = services.world.lightVolumes.tryGet(entity);
    if (volume == nullptr) return false;
    editLightField(
        services,
        entity,
        "HalfExtents",
        "Resize Light Volume",
        *volume,
        [](LightVolumeComponent& edited) {
            return ImGui::DragFloat3(
                "Half Extents",
                glm::value_ptr(edited.halfExtents),
                0.05f,
                0.01f,
                1000.0f
            );
        },
        sanitizeVolume
    );
    return true;
}

bool drawPointLight(EngineServices& services, EntityId entity) {
    PointLightComponent* light = services.world.pointLights.tryGet(entity);
    if (light == nullptr) return false;
    const auto edit = [&](const char* id, const char* label, auto draw) {
        editLightField(services, entity, id, label, *light, draw, sanitizePoint);
    };
    edit("Radius", "Edit Point Light Radius", [](PointLightComponent& value) {
        return ImGui::DragFloat("Radius", &value.radius, 0.1f, 0.01f, 128.0f);
    });
    edit("Color", "Edit Point Light Color", [](PointLightComponent& value) {
        return ImGui::ColorEdit3("Color", glm::value_ptr(value.color));
    });
    edit("Intensity", "Edit Point Light Intensity", [](PointLightComponent& value) {
        return ImGui::DragFloat("Intensity", &value.intensity, 0.1f, 0.0f, 128.0f);
    });
    edit("Phase", "Edit Point Light Phase", [](PointLightComponent& value) {
        return ImGui::DragFloat("Phase", &value.phase, 0.01f);
    });
    edit("Movable", "Toggle Point Light Mobility", [](PointLightComponent& value) {
        return ImGui::Checkbox("Movable", &value.isMovable);
    });
    edit("CastsShadow", "Toggle Point Light Shadows", [](PointLightComponent& value) {
        return ImGui::Checkbox("Casts Shadow", &value.castsShadow);
    });
    edit("ShadowBiasMin", "Edit Point Light Minimum Bias", [](PointLightComponent& value) {
        return ImGui::DragFloat("Shadow Bias Min", &value.shadowBiasMin, 0.00001f, 0.0f, 0.1f, "%.6f");
    });
    edit("ShadowBiasSlope", "Edit Point Light Slope Bias", [](PointLightComponent& value) {
        return ImGui::DragFloat("Shadow Bias Slope", &value.shadowBiasSlope, 0.0001f, 0.0f, 0.1f, "%.5f");
    });
    return true;
}

bool drawSpotLight(EngineServices& services, EntityId entity) {
    SpotLightComponent* light = services.world.spotLights.tryGet(entity);
    if (light == nullptr) return false;
    const auto edit = [&](const char* id, const char* label, auto draw) {
        editLightField(services, entity, id, label, *light, draw, sanitizeSpot);
    };
    edit("Radius", "Edit Spot Light Radius", [](SpotLightComponent& value) {
        return ImGui::DragFloat("Radius", &value.radius, 0.1f, 0.01f, 128.0f);
    });
    edit("Color", "Edit Spot Light Color", [](SpotLightComponent& value) {
        return ImGui::ColorEdit3("Color", glm::value_ptr(value.color));
    });
    edit("Intensity", "Edit Spot Light Intensity", [](SpotLightComponent& value) {
        return ImGui::DragFloat("Intensity", &value.intensity, 0.1f, 0.0f, 128.0f);
    });
    edit("Target", "Aim Spot Light", [](SpotLightComponent& value) {
        return ImGui::DragFloat3("Target", glm::value_ptr(value.target), 0.05f);
    });
    edit("InnerAngle", "Edit Spot Light Inner Angle", [](SpotLightComponent& value) {
        return ImGui::DragFloat("Inner Angle", &value.innerAngle, 0.25f, 0.0f, value.outerAngle);
    });
    edit("OuterAngle", "Edit Spot Light Outer Angle", [](SpotLightComponent& value) {
        return ImGui::DragFloat("Outer Angle", &value.outerAngle, 0.25f, value.innerAngle, 179.0f);
    });
    edit("Phase", "Edit Spot Light Phase", [](SpotLightComponent& value) {
        return ImGui::DragFloat("Phase", &value.phase, 0.01f);
    });
    edit("Movable", "Toggle Spot Light Mobility", [](SpotLightComponent& value) {
        return ImGui::Checkbox("Movable", &value.isMovable);
    });
    edit("CastsShadow", "Toggle Spot Light Shadows", [](SpotLightComponent& value) {
        return ImGui::Checkbox("Casts Shadow", &value.castsShadow);
    });
    edit("ShadowBiasMin", "Edit Spot Light Minimum Bias", [](SpotLightComponent& value) {
        return ImGui::DragFloat("Shadow Bias Min", &value.shadowBiasMin, 0.00001f, 0.0f, 0.1f, "%.6f");
    });
    edit("ShadowBiasSlope", "Edit Spot Light Slope Bias", [](SpotLightComponent& value) {
        return ImGui::DragFloat("Shadow Bias Slope", &value.shadowBiasSlope, 0.0001f, 0.0f, 0.1f, "%.5f");
    });
    return true;
}

}  // namespace

void ComponentRegistry::registerLocalLightingDescriptors() {
    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::LightVolume,
        "Light Volume",
        "Lighting",
        [](const World& world, EntityId entity) { return world.lightVolumes.contains(entity); },
        [](World& world, EntityId entity) { world.lightVolumes.emplace(entity, LightVolumeComponent{}); },
        [](World& world, EntityId entity) { world.lightVolumes.remove(entity); },
        drawLightVolume,
    });
    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::PointLight,
        "Point Light",
        "Lighting",
        [](const World& world, EntityId entity) { return world.pointLights.contains(entity); },
        [](World& world, EntityId entity) { world.pointLights.emplace(entity, PointLightComponent{}); },
        [](World& world, EntityId entity) { world.pointLights.remove(entity); },
        drawPointLight,
    });
    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::SpotLight,
        "Spot Light",
        "Lighting",
        [](const World& world, EntityId entity) { return world.spotLights.contains(entity); },
        [](World& world, EntityId entity) { world.spotLights.emplace(entity, SpotLightComponent{}); },
        [](World& world, EntityId entity) { world.spotLights.remove(entity); },
        drawSpotLight,
    });
}

}  // namespace core
