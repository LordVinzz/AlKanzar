#include "RenderExtractionSystem.hpp"

#include <algorithm>
#include <vector>

namespace core {

namespace {

std::size_t taskGrain(std::size_t count, std::size_t workerCount) {
    const std::size_t lanes = std::max<std::size_t>(1u, workerCount + 1u) * 2u;
    return std::max<std::size_t>(32u, (count + lanes - 1u) / lanes);
}

}  // namespace

void RenderExtractionSystem::extract(
    const World& world,
    const MaterialLibrary& materials,
    const SelectionModel& selection,
    FrameSceneData& outFrame,
    TaskScheduler& scheduler,
    bool useParallel
) const {
    outFrame.clear();
    std::vector<int> lightIndexByEntity(world.lightRuntime_.size(), -1);

    if (!useParallel) {
        outFrame.reserve(world.renderables.size(), world.pointLights.size() + world.spotLights.size(), world.lightVolumes.size());

        for (EntityId entity : world.renderables.entities()) {
            const RenderableComponent& renderable = world.renderables.get(entity);
            const VisibilityComponent* visibility = world.visibilities.tryGet(entity);
            const bool visible = visibility == nullptr || visibility->visible;
            const TransformCacheEntry* cache = entity.index < world.transformCache_.size()
                ? &world.transformCache_[entity.index]
                : nullptr;

            FrameRenderable frameRenderable{};
            frameRenderable.entity = entity;
            frameRenderable.mesh = renderable.mesh;
            frameRenderable.materialHandle = renderable.material;
            frameRenderable.material = materials.get(renderable.material);
            frameRenderable.layer = renderable.layer;
            frameRenderable.visible = visible;
            frameRenderable.localBounds = world.bounds.contains(entity)
                ? world.bounds.get(entity).localBounds
                : render::Bounds3{};
            if (cache != nullptr) {
                frameRenderable.worldBounds = cache->worldBounds;
                frameRenderable.modelMatrix = cache->worldMatrix;
            }
            outFrame.renderables.push_back(std::move(frameRenderable));
        }

        for (EntityId entity : world.pointLights.entities()) {
            const PointLightComponent& light = world.pointLights.get(entity);
            const LightRuntime& runtime = world.lightRuntime_[entity.index];
            FrameLight frameLight{};
            frameLight.entity = entity;
            frameLight.type = render::LightType::Point;
            frameLight.position = runtime.position;
            frameLight.radius = light.radius;
            frameLight.color = light.color;
            frameLight.intensity = light.intensity;
            frameLight.direction = runtime.direction;
            frameLight.isMovable = light.isMovable;
            frameLight.castsShadow = light.castsShadow;
            frameLight.shadowBiasMin = light.shadowBiasMin;
            frameLight.shadowBiasSlope = light.shadowBiasSlope;
            lightIndexByEntity[entity.index] = static_cast<int>(outFrame.lights.size());
            outFrame.lights.push_back(std::move(frameLight));
        }

        for (EntityId entity : world.spotLights.entities()) {
            const SpotLightComponent& light = world.spotLights.get(entity);
            const LightRuntime& runtime = world.lightRuntime_[entity.index];
            FrameLight frameLight{};
            frameLight.entity = entity;
            frameLight.type = render::LightType::Spot;
            frameLight.position = runtime.position;
            frameLight.radius = light.radius;
            frameLight.color = light.color;
            frameLight.intensity = light.intensity;
            frameLight.direction = runtime.direction;
            frameLight.innerAngle = light.innerAngle;
            frameLight.outerAngle = light.outerAngle;
            frameLight.isMovable = light.isMovable;
            frameLight.castsShadow = light.castsShadow;
            frameLight.shadowBiasMin = light.shadowBiasMin;
            frameLight.shadowBiasSlope = light.shadowBiasSlope;
            lightIndexByEntity[entity.index] = static_cast<int>(outFrame.lights.size());
            outFrame.lights.push_back(std::move(frameLight));
        }

        for (const LightVolume& volume : world.lightVolumes) {
            FrameLightVolume frameVolume{};
            frameVolume.minCorner = volume.minCorner();
            frameVolume.maxCorner = volume.maxCorner();
            for (EntityId entity : volume.staticLightEntities()) {
                if (entity.index < lightIndexByEntity.size() && lightIndexByEntity[entity.index] >= 0) {
                    frameVolume.staticLightIndices.push_back(lightIndexByEntity[entity.index]);
                }
            }
            for (EntityId entity : volume.movableLightEntities()) {
                if (entity.index < lightIndexByEntity.size() && lightIndexByEntity[entity.index] >= 0) {
                    frameVolume.movableLightIndices.push_back(lightIndexByEntity[entity.index]);
                }
            }
            outFrame.lightVolumes.push_back(std::move(frameVolume));
        }

    } else {
        outFrame.renderables.resize(world.renderables.size());
        outFrame.lights.resize(world.pointLights.size() + world.spotLights.size());
        outFrame.lightVolumes.resize(world.lightVolumes.size());

        TaskGroup primaryGroup;
        scheduler.parallelFor(
            primaryGroup,
            world.renderables.size(),
            taskGrain(world.renderables.size(), scheduler.workerCount()),
            "Render Extract Renderables",
            [&](std::size_t begin, std::size_t end) {
                const std::vector<EntityId>& entities = world.renderables.entities();
                for (std::size_t index = begin; index < end; ++index) {
                    const EntityId entity = entities[index];
                    const RenderableComponent& renderable = world.renderables.get(entity);
                    const VisibilityComponent* visibility = world.visibilities.tryGet(entity);
                    const TransformCacheEntry* cache = entity.index < world.transformCache_.size()
                        ? &world.transformCache_[entity.index]
                        : nullptr;

                    FrameRenderable frameRenderable{};
                    frameRenderable.entity = entity;
                    frameRenderable.mesh = renderable.mesh;
                    frameRenderable.materialHandle = renderable.material;
                    frameRenderable.material = materials.get(renderable.material);
                    frameRenderable.layer = renderable.layer;
                    frameRenderable.visible = visibility == nullptr || visibility->visible;
                    frameRenderable.localBounds = world.bounds.contains(entity)
                        ? world.bounds.get(entity).localBounds
                        : render::Bounds3{};
                    if (cache != nullptr) {
                        frameRenderable.worldBounds = cache->worldBounds;
                        frameRenderable.modelMatrix = cache->worldMatrix;
                    }
                    outFrame.renderables[index] = std::move(frameRenderable);
                }
            }
        );

        scheduler.parallelFor(
            primaryGroup,
            world.pointLights.size(),
            taskGrain(world.pointLights.size(), scheduler.workerCount()),
            "Render Extract Point Lights",
            [&](std::size_t begin, std::size_t end) {
                const std::vector<EntityId>& entities = world.pointLights.entities();
                for (std::size_t index = begin; index < end; ++index) {
                    const EntityId entity = entities[index];
                    const PointLightComponent& light = world.pointLights.get(entity);
                    const LightRuntime& runtime = world.lightRuntime_[entity.index];
                    FrameLight frameLight{};
                    frameLight.entity = entity;
                    frameLight.type = render::LightType::Point;
                    frameLight.position = runtime.position;
                    frameLight.radius = light.radius;
                    frameLight.color = light.color;
                    frameLight.intensity = light.intensity;
                    frameLight.direction = runtime.direction;
                    frameLight.isMovable = light.isMovable;
                    frameLight.castsShadow = light.castsShadow;
                    frameLight.shadowBiasMin = light.shadowBiasMin;
                    frameLight.shadowBiasSlope = light.shadowBiasSlope;
                    outFrame.lights[index] = std::move(frameLight);
                    lightIndexByEntity[entity.index] = static_cast<int>(index);
                }
            }
        );

        const std::size_t spotOffset = world.pointLights.size();
        scheduler.parallelFor(
            primaryGroup,
            world.spotLights.size(),
            taskGrain(world.spotLights.size(), scheduler.workerCount()),
            "Render Extract Spot Lights",
            [&](std::size_t begin, std::size_t end) {
                const std::vector<EntityId>& entities = world.spotLights.entities();
                for (std::size_t index = begin; index < end; ++index) {
                    const EntityId entity = entities[index];
                    const SpotLightComponent& light = world.spotLights.get(entity);
                    const LightRuntime& runtime = world.lightRuntime_[entity.index];
                    FrameLight frameLight{};
                    frameLight.entity = entity;
                    frameLight.type = render::LightType::Spot;
                    frameLight.position = runtime.position;
                    frameLight.radius = light.radius;
                    frameLight.color = light.color;
                    frameLight.intensity = light.intensity;
                    frameLight.direction = runtime.direction;
                    frameLight.innerAngle = light.innerAngle;
                    frameLight.outerAngle = light.outerAngle;
                    frameLight.isMovable = light.isMovable;
                    frameLight.castsShadow = light.castsShadow;
                    frameLight.shadowBiasMin = light.shadowBiasMin;
                    frameLight.shadowBiasSlope = light.shadowBiasSlope;
                    outFrame.lights[spotOffset + index] = std::move(frameLight);
                    lightIndexByEntity[entity.index] = static_cast<int>(spotOffset + index);
                }
            }
        );
        scheduler.wait(primaryGroup);

        TaskGroup volumeGroup;
        scheduler.parallelFor(
            volumeGroup,
            world.lightVolumes.size(),
            taskGrain(world.lightVolumes.size(), scheduler.workerCount()),
            "Render Extract Light Volumes",
            [&](std::size_t begin, std::size_t end) {
                for (std::size_t volumeIndex = begin; volumeIndex < end; ++volumeIndex) {
                    const LightVolume& volume = world.lightVolumes[volumeIndex];
                    FrameLightVolume frameVolume{};
                    frameVolume.minCorner = volume.minCorner();
                    frameVolume.maxCorner = volume.maxCorner();
                    for (EntityId entity : volume.staticLightEntities()) {
                        if (entity.index < lightIndexByEntity.size() && lightIndexByEntity[entity.index] >= 0) {
                            frameVolume.staticLightIndices.push_back(lightIndexByEntity[entity.index]);
                        }
                    }
                    for (EntityId entity : volume.movableLightEntities()) {
                        if (entity.index < lightIndexByEntity.size() && lightIndexByEntity[entity.index] >= 0) {
                            frameVolume.movableLightIndices.push_back(lightIndexByEntity[entity.index]);
                        }
                    }
                    outFrame.lightVolumes[volumeIndex] = std::move(frameVolume);
                }
            }
        );
        scheduler.wait(volumeGroup);
    }

    if (!selection.current().has_value()) {
        return;
    }

    const EntityId selected = *selection.current();
    outFrame.selection.entity = selected;
    outFrame.selection.isLight = world.pointLights.contains(selected) || world.spotLights.contains(selected);
    if (selected.index < world.transformCache_.size()) {
        outFrame.selection.worldBounds = world.transformCache_[selected.index].worldBounds;
        outFrame.selection.hasWorldBounds = world.transformCache_[selected.index].hasWorldBounds;
    }

    const EntityId transformEntity = world.editableTransformEntity(selected);
    if (transformEntity.valid() && transformEntity.index < world.transformCache_.size()) {
        outFrame.selection.transformMatrix = world.transformCache_[transformEntity.index].worldMatrix;
    }
}

}  // namespace core
