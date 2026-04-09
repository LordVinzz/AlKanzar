#include "LightSystem.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>

namespace core {

namespace {

struct LightAssignment {
    EntityId entity{};
    float radius{0.0f};
    bool isMovable{false};
};

struct ChunkVolumeAssignments {
    std::vector<std::vector<EntityId>> staticByVolume{};
    std::vector<std::vector<EntityId>> movableByVolume{};
};

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

    for (auto& volume : world.lightVolumes) {
        volume.clearStaticLights();
        volume.clearMovableLights();
    }

    std::vector<LightAssignment> assignments{};
    assignments.reserve(world.pointLights.size() + world.spotLights.size());
    for (EntityId entity : world.pointLights.entities()) {
        const PointLightComponent& light = world.pointLights.get(entity);
        assignments.push_back(LightAssignment{entity, light.radius, light.isMovable});
    }
    for (EntityId entity : world.spotLights.entities()) {
        const SpotLightComponent& light = world.spotLights.get(entity);
        assignments.push_back(LightAssignment{entity, light.radius, light.isMovable});
    }
    std::sort(assignments.begin(), assignments.end(), [](const LightAssignment& lhs, const LightAssignment& rhs) {
        if (lhs.entity.index != rhs.entity.index) {
            return lhs.entity.index < rhs.entity.index;
        }
        return lhs.entity.generation < rhs.entity.generation;
    });

    if (!useParallel) {
        for (const LightAssignment& assignment : assignments) {
            const LightRuntime& runtime = world.lightRuntime_[assignment.entity.index];
            if (!runtime.valid) {
                continue;
            }

            for (auto& volume : world.lightVolumes) {
                if (!volume.intersectsSphere(runtime.position, assignment.radius)) {
                    continue;
                }

                if (assignment.isMovable) {
                    volume.addMovableLight(assignment.entity);
                } else {
                    volume.attachStaticLight(assignment.entity);
                }
            }
        }
    } else {
        const std::size_t assignmentGrain = taskGrain(assignments.size(), scheduler.workerCount());
        const std::size_t chunkCount =
            assignmentGrain == 0u ? 0u : (assignments.size() + assignmentGrain - 1u) / assignmentGrain;
        std::vector<ChunkVolumeAssignments> chunkAssignments(chunkCount);
        TaskGroup assignmentGroup;
        for (std::size_t chunkIndex = 0u; chunkIndex < chunkCount; ++chunkIndex) {
            const std::size_t begin = chunkIndex * assignmentGrain;
            const std::size_t end = std::min(assignments.size(), begin + assignmentGrain);
            scheduler.schedule(assignmentGroup, "Light Volume Assignment", [&, chunkIndex, begin, end]() {
                ChunkVolumeAssignments& chunk = chunkAssignments[chunkIndex];
                chunk.staticByVolume.resize(world.lightVolumes.size());
                chunk.movableByVolume.resize(world.lightVolumes.size());

                for (std::size_t lightIndex = begin; lightIndex < end; ++lightIndex) {
                    const LightAssignment& assignment = assignments[lightIndex];
                    const LightRuntime& runtime = world.lightRuntime_[assignment.entity.index];
                    if (!runtime.valid) {
                        continue;
                    }

                    for (std::size_t volumeIndex = 0u; volumeIndex < world.lightVolumes.size(); ++volumeIndex) {
                        if (!world.lightVolumes[volumeIndex].intersectsSphere(runtime.position, assignment.radius)) {
                            continue;
                        }

                        if (assignment.isMovable) {
                            chunk.movableByVolume[volumeIndex].push_back(assignment.entity);
                        } else {
                            chunk.staticByVolume[volumeIndex].push_back(assignment.entity);
                        }
                    }
                }
            });
        }
        scheduler.wait(assignmentGroup);

        for (std::size_t volumeIndex = 0u; volumeIndex < world.lightVolumes.size(); ++volumeIndex) {
            for (const ChunkVolumeAssignments& chunk : chunkAssignments) {
                for (EntityId entity : chunk.staticByVolume[volumeIndex]) {
                    world.lightVolumes[volumeIndex].attachStaticLight(entity);
                }
                for (EntityId entity : chunk.movableByVolume[volumeIndex]) {
                    world.lightVolumes[volumeIndex].addMovableLight(entity);
                }
            }
        }
    }

    world.clearLightDirty();
}

}  // namespace core
