#include "ComponentRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include "ComponentInspector.hpp"
#include "core/app/EngineServices.hpp"
#include "core/ecs/World.hpp"

namespace core {
namespace {

DirectionalLightComponent sanitizeDirectionalLight(DirectionalLightComponent light) {
    const float directionLength = glm::length(light.direction);
    if (directionLength > 1.0e-6f && std::isfinite(directionLength)) {
        light.direction /= directionLength;
    } else {
        light.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    }

    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(light.color[axis])) {
            light.color[axis] = 0.0f;
        }
        light.color[axis] = std::max(light.color[axis], 0.0f);
    }
    if (!std::isfinite(light.intensity)) {
        light.intensity = 0.0f;
    }
    light.intensity = std::max(light.intensity, 0.0f);
    return light;
}

std::string directionalLightMergeKey(EntityId entity, const char* field) {
    return "directional-light-" + std::to_string(entity.index) + "-" +
        std::to_string(entity.generation) + "-" + field;
}

void applyDirectionalLightSnapshot(
    EngineServices& services,
    EntityId entity,
    const DirectionalLightComponent& snapshot
) {
    DirectionalLightComponent* light = services.world.directionalLights.tryGet(entity);
    if (light == nullptr) {
        return;
    }
    *light = sanitizeDirectionalLight(snapshot);
    notifyLightChanged(services, entity);
}

}  // namespace

void ComponentRegistry::registerDirectionalLightDescriptor() {
    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::DirectionalLight,
        "Directional Light",
        "Lighting",
        [](const World& world, EntityId entity) {
            return world.directionalLights.contains(entity);
        },
        [](World& world, EntityId entity) {
            if (world.directionalLights.entities().empty()) {
                world.directionalLights.emplace(
                    entity,
                    sanitizeDirectionalLight(DirectionalLightComponent{})
                );
            }
        },
        [](World& world, EntityId entity) {
            world.directionalLights.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            DirectionalLightComponent* light = services.world.directionalLights.tryGet(entity);
            if (light == nullptr) {
                return false;
            }

            const auto applySnapshot = [&services, entity](const DirectionalLightComponent& snapshot) {
                applyDirectionalLightSnapshot(services, entity, snapshot);
            };
            editComponentSnapshot<DirectionalLightComponent>(
                "Direction",
                "Edit Directional Light Direction",
                directionalLightMergeKey(entity, "direction"),
                *light,
                applySnapshot,
                [](DirectionalLightComponent& edited) {
                    const bool changed = ImGui::DragFloat3(
                        "Direction",
                        glm::value_ptr(edited.direction),
                        0.01f,
                        -1.0f,
                        1.0f,
                        "%.3f"
                    );
                    if (changed) {
                        edited = sanitizeDirectionalLight(edited);
                    }
                    return changed;
                },
                services.commands
            );
            editComponentSnapshot<DirectionalLightComponent>(
                "Color",
                "Edit Directional Light Color",
                directionalLightMergeKey(entity, "color"),
                *light,
                applySnapshot,
                [](DirectionalLightComponent& edited) {
                    return ImGui::ColorEdit3("Color", glm::value_ptr(edited.color));
                },
                services.commands
            );
            editComponentSnapshot<DirectionalLightComponent>(
                "Intensity",
                "Edit Directional Light Intensity",
                directionalLightMergeKey(entity, "intensity"),
                *light,
                applySnapshot,
                [](DirectionalLightComponent& edited) {
                    return ImGui::DragFloat(
                        "Intensity",
                        &edited.intensity,
                        0.05f,
                        0.0f,
                        128.0f,
                        "%.2f"
                    );
                },
                services.commands
            );
            ImGui::TextDisabled("Global scene light; excluded from local light volumes.");
            return true;
        },
    });
}

}  // namespace core
