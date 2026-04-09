#include "LightSystem.hpp"

#include <algorithm>
#include <cmath>

namespace core {

namespace {

void evaluatePointLight(
    const TransformComponent* transform,
    const PointLightComponent& light,
    float timeSeconds,
    LightRuntime& runtime
) {
    runtime.position = transform ? transform->position : glm::vec3(0.0f);
    runtime.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    runtime.type = render::LightType::Point;
    runtime.isMovable = light.isMovable;
    runtime.valid = true;

    if (!light.isMovable) {
        return;
    }

    const float phase = light.phase + timeSeconds;
    runtime.position.x += 0.55f * std::cos(phase * 0.7f);
    runtime.position.z += 0.55f * std::sin(phase * 0.9f);
    runtime.position.y += 0.35f * std::sin(phase * 1.3f);
}

void evaluateSpotLight(
    const TransformComponent* transform,
    const SpotLightComponent& light,
    float timeSeconds,
    LightRuntime& runtime
) {
    runtime.position = transform ? transform->position : glm::vec3(0.0f);
    runtime.type = render::LightType::Spot;
    runtime.isMovable = light.isMovable;
    runtime.valid = true;

    if (light.isMovable) {
        const float phase = light.phase + timeSeconds;
        runtime.position.x += 2.25f * std::cos(phase * 0.7f);
        runtime.position.z += 2.25f * std::sin(phase * 0.9f);
        runtime.position.y += 2.15f * std::sin(phase * 1.3f);
    }

    runtime.direction = glm::normalize(light.target - runtime.position);
}

}  // namespace

void LightSystem::update(World& world, const TimeContext& timeContext) const {
    std::fill(world.lightRuntime_.begin(), world.lightRuntime_.end(), LightRuntime{});

    for (EntityId entity : world.pointLights.entities()) {
        world.ensureCacheSize(entity.index + 1u);
        evaluatePointLight(
            world.transforms.tryGet(entity),
            world.pointLights.get(entity),
            timeContext.totalSeconds,
            world.lightRuntime_[entity.index]
        );
    }

    for (EntityId entity : world.spotLights.entities()) {
        world.ensureCacheSize(entity.index + 1u);
        evaluateSpotLight(
            world.transforms.tryGet(entity),
            world.spotLights.get(entity),
            timeContext.totalSeconds,
            world.lightRuntime_[entity.index]
        );
    }

    for (auto& volume : world.lightVolumes) {
        volume.clearStaticLights();
        volume.clearMovableLights();
    }

    auto assignLight = [&world](EntityId entity, float radius, bool isMovable) {
        const LightRuntime& runtime = world.lightRuntime_[entity.index];
        if (!runtime.valid) {
            return;
        }

        for (auto& volume : world.lightVolumes) {
            if (!volume.intersectsSphere(runtime.position, radius)) {
                continue;
            }
            if (isMovable) {
                volume.addMovableLight(entity);
            } else {
                volume.attachStaticLight(entity);
            }
        }
    };

    for (EntityId entity : world.pointLights.entities()) {
        const PointLightComponent& light = world.pointLights.get(entity);
        assignLight(entity, light.radius, light.isMovable);
    }

    for (EntityId entity : world.spotLights.entities()) {
        const SpotLightComponent& light = world.spotLights.get(entity);
        assignLight(entity, light.radius, light.isMovable);
    }

    world.clearLightDirty();
}

}  // namespace core
