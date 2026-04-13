#include "LightSystem.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>

namespace core {

namespace {

std::size_t taskGrain(std::size_t count, std::size_t workerCount) {
    const std::size_t lanes = std::max<std::size_t>(1u, workerCount + 1u) * 2u;
    return std::max<std::size_t>(8u, (count + lanes - 1u) / lanes);
}

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

void LightSystem::update(
    World& world,
    const TimeContext& timeContext,
    TaskScheduler& scheduler,
    bool useParallel
) const {
    std::size_t maxIndex = world.lightRuntime_.size();
    for (EntityId entity : world.pointLights.entities()) {
        maxIndex = std::max(maxIndex, static_cast<std::size_t>(entity.index + 1u));
    }
    for (EntityId entity : world.spotLights.entities()) {
        maxIndex = std::max(maxIndex, static_cast<std::size_t>(entity.index + 1u));
    }
    world.ensureCacheSize(maxIndex);
    std::fill(world.lightRuntime_.begin(), world.lightRuntime_.end(), LightRuntime{});

    if (!useParallel) {
        for (EntityId entity : world.pointLights.entities()) {
            evaluatePointLight(
                world.transforms.tryGet(entity),
                world.pointLights.get(entity),
                timeContext.totalSeconds,
                world.lightRuntime_[entity.index]
            );
        }
        for (EntityId entity : world.spotLights.entities()) {
            evaluateSpotLight(
                world.transforms.tryGet(entity),
                world.spotLights.get(entity),
                timeContext.totalSeconds,
                world.lightRuntime_[entity.index]
            );
        }
    } else {
        TaskGroup runtimeGroup;
        scheduler.parallelFor(
            runtimeGroup,
            world.pointLights.size(),
            taskGrain(world.pointLights.size(), scheduler.workerCount()),
            "Point Light Runtime",
            [&](std::size_t begin, std::size_t end) {
                const std::vector<EntityId>& entities = world.pointLights.entities();
                for (std::size_t index = begin; index < end; ++index) {
                    const EntityId entity = entities[index];
                    evaluatePointLight(
                        world.transforms.tryGet(entity),
                        world.pointLights.get(entity),
                        timeContext.totalSeconds,
                        world.lightRuntime_[entity.index]
                    );
                }
            }
        );
        scheduler.parallelFor(
            runtimeGroup,
            world.spotLights.size(),
            taskGrain(world.spotLights.size(), scheduler.workerCount()),
            "Spot Light Runtime",
            [&](std::size_t begin, std::size_t end) {
                const std::vector<EntityId>& entities = world.spotLights.entities();
                for (std::size_t index = begin; index < end; ++index) {
                    const EntityId entity = entities[index];
                    evaluateSpotLight(
                        world.transforms.tryGet(entity),
                        world.spotLights.get(entity),
                        timeContext.totalSeconds,
                        world.lightRuntime_[entity.index]
                    );
                }
            }
        );
        scheduler.wait(runtimeGroup);
    }

    world.clearLightDirty();
}

}  // namespace core
