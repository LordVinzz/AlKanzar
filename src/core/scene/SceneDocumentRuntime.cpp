#include "SceneDocument.hpp"

#include <optional>
#include <string_view>

#include "core/ecs/World.hpp"

namespace core {

bool SceneDocument::captureRuntimeObjectValues(std::string_view id, const World& world) {
    const std::optional<SceneObjectHandle> handle = findObject(id);
    const std::optional<EntityId> entity = entityForObject(id);
    if (!handle.has_value() || !entity.has_value() || !world.isAlive(*entity)) {
        return false;
    }

    if (const NameComponent* name = world.names.tryGet(*entity); name != nullptr && !name->value.empty()) {
        switch (handle->type) {
            case SceneObjectType::Primitive: blueprint_.primitives[handle->index].name = name->value; break;
            case SceneObjectType::Model: blueprint_.models[handle->index].name = name->value; break;
            case SceneObjectType::DirectionalLight: blueprint_.directionalLight->name = name->value; break;
            case SceneObjectType::LightVolume: blueprint_.lightVolumes[handle->index].name = name->value; break;
            case SceneObjectType::PointLight: blueprint_.pointLights[handle->index].name = name->value; break;
            case SceneObjectType::SpotLight: blueprint_.spotLights[handle->index].name = name->value; break;
        }
    }
    switch (handle->type) {
        case SceneObjectType::Primitive: {
            ScenePrimitiveBlueprint& primitive = blueprint_.primitives[handle->index];
            if (const TransformComponent* transform = world.transforms.tryGet(*entity)) {
                primitive.transform = *transform;
            }
            break;
        }
        case SceneObjectType::Model: {
            ModelInstanceBlueprint& model = blueprint_.models[handle->index];
            if (const TransformComponent* transform = world.transforms.tryGet(*entity)) {
                model.transform = *transform;
            }
            if (const CharacterComponent* character = world.characters.tryGet(*entity)) {
                CharacterBlueprint captured = model.character.value_or(CharacterBlueprint{});
                captured.character = *character;
                if (const CharacterControllerComponent* controller = world.characterControllers.tryGet(*entity)) {
                    captured.controller = *controller;
                }
                if (const PartyMemberComponent* partyMember = world.partyMembers.tryGet(*entity)) {
                    captured.partyMember = *partyMember;
                } else {
                    captured.partyMember.reset();
                }
                if (const AbilityScoresComponent* abilities = world.abilityScores.tryGet(*entity)) {
                    captured.abilities = *abilities;
                }
                if (const SkillRanksComponent* skills = world.skillRanks.tryGet(*entity)) {
                    captured.skills = *skills;
                }
                if (const CharacterVitalsComponent* vitals = world.characterVitals.tryGet(*entity)) {
                    captured.vitals = *vitals;
                }
                model.character = std::move(captured);
            }
            break;
        }
        case SceneObjectType::DirectionalLight: {
            if (const DirectionalLightComponent* runtime = world.directionalLights.tryGet(*entity)) {
                DirectionalLightBlueprint& light = *blueprint_.directionalLight;
                light.direction = runtime->direction;
                light.color = runtime->color;
                light.intensity = runtime->intensity;
            }
            break;
        }
        case SceneObjectType::LightVolume: {
            LightVolumeBlueprint& volume = blueprint_.lightVolumes[handle->index];
            if (const TransformComponent* transform = world.transforms.tryGet(*entity)) {
                volume.transform = *transform;
            }
            if (const LightVolumeComponent* runtime = world.lightVolumes.tryGet(*entity)) {
                volume.halfExtents = runtime->halfExtents;
            }
            break;
        }
        case SceneObjectType::PointLight: {
            PointLightBlueprint& light = blueprint_.pointLights[handle->index];
            if (const TransformComponent* transform = world.transforms.tryGet(*entity)) {
                light.transform = *transform;
            }
            if (const PointLightComponent* runtime = world.pointLights.tryGet(*entity)) {
                light.radius = runtime->radius;
                light.color = runtime->color;
                light.intensity = runtime->intensity;
                light.phase = runtime->phase;
                light.isMovable = runtime->isMovable;
                light.castsShadow = runtime->castsShadow;
                light.shadowBiasMin = runtime->shadowBiasMin;
                light.shadowBiasSlope = runtime->shadowBiasSlope;
            }
            break;
        }
        case SceneObjectType::SpotLight: {
            SpotLightBlueprint& light = blueprint_.spotLights[handle->index];
            if (const TransformComponent* transform = world.transforms.tryGet(*entity)) {
                light.transform = *transform;
            }
            if (const SpotLightComponent* runtime = world.spotLights.tryGet(*entity)) {
                light.radius = runtime->radius;
                light.color = runtime->color;
                light.intensity = runtime->intensity;
                light.target = runtime->target;
                light.innerAngle = runtime->innerAngle;
                light.outerAngle = runtime->outerAngle;
                light.phase = runtime->phase;
                light.isMovable = runtime->isMovable;
                light.castsShadow = runtime->castsShadow;
                light.shadowBiasMin = runtime->shadowBiasMin;
                light.shadowBiasSlope = runtime->shadowBiasSlope;
            }
            break;
        }
    }
    return true;
}

bool SceneDocument::captureRuntimeObject(std::string_view id, const World& world) {
    if (!captureRuntimeObjectValues(id, world)) {
        return false;
    }
    refreshDirty();
    return true;
}

void SceneDocument::captureRuntimeWorld(const World& world) {
    bool captured = false;
    for (const auto& [id, entity] : entityByObject_) {
        if (world.isAlive(entity)) {
            captured |= captureRuntimeObjectValues(id, world);
        }
    }
    if (captured) {
        refreshDirty();
    }
}

}  // namespace core
