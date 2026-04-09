#include "RenderExtractionSystem.hpp"

#include <vector>

namespace core {

void RenderExtractionSystem::extract(
    const World& world,
    const MaterialLibrary& materials,
    const SelectionModel& selection,
    FrameSceneData& outFrame
) const {
    outFrame.clear();
    outFrame.reserve(world.renderables.size(), world.pointLights.size() + world.spotLights.size(), world.lightVolumes.size());

    std::vector<int> lightIndexByEntity(world.lightRuntime_.size(), -1);

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
