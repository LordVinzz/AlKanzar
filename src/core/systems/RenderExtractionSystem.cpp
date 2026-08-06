#include "RenderExtractionSystem.hpp"
#include "RenderExtractionAnimation.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>

#include "core/presentation/CharacterPresentation.hpp"
#include "core/transform/TransformMath.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace core {

namespace {

std::size_t taskGrain(std::size_t count, std::size_t workerCount) {
    const std::size_t lanes = std::max<std::size_t>(1u, workerCount + 1u) * 2u;
    return std::max<std::size_t>(32u, (count + lanes - 1u) / lanes);
}

render::Bounds3 mergeBounds(const render::Bounds3& lhs, const render::Bounds3& rhs) {
    return render::Bounds3{
        glm::min(lhs.min, rhs.min),
        glm::max(lhs.max, rhs.max),
    };
}

bool endsWithIgnoreCase(std::string_view value, std::string_view suffix) {
    if (value.size() < suffix.size()) {
        return false;
    }

    const std::size_t offset = value.size() - suffix.size();
    for (std::size_t index = 0; index < suffix.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(value[offset + index])) !=
            std::tolower(static_cast<unsigned char>(suffix[index]))) {
            return false;
        }
    }
    return true;
}

glm::mat4 worldMatrixForEntity(const World& world, EntityId entity) {
    if (entity.index < world.transformCache_.size() && world.transformCache_[entity.index].valid) {
        return world.transformCache_[entity.index].worldMatrix;
    }
    if (const TransformComponent* transform = world.transforms.tryGet(entity)) {
        return composeTransform(*transform);
    }
    return glm::mat4(1.0f);
}

render::Bounds3 centeredBounds(const glm::vec3& center, const glm::vec3& halfExtents) {
    return render::Bounds3{
        center - halfExtents,
        center + halfExtents,
    };
}

bool intersectsSphere(const render::Bounds3& bounds, const glm::vec3& center, float radius) {
    float distanceSquared = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        const float value = center[axis];
        if (value < bounds.min[axis]) {
            const float delta = bounds.min[axis] - value;
            distanceSquared += delta * delta;
        } else if (value > bounds.max[axis]) {
            const float delta = value - bounds.max[axis];
            distanceSquared += delta * delta;
        }
    }
    return distanceSquared <= radius * radius;
}

float maxAxisScale(const glm::mat4& modelMatrix) {
    const float xScale = glm::length(glm::vec3(modelMatrix[0]));
    const float yScale = glm::length(glm::vec3(modelMatrix[1]));
    const float zScale = glm::length(glm::vec3(modelMatrix[2]));
    return std::max(xScale, std::max(yScale, zScale));
}

}  // namespace

void RenderExtractionSystem::extract(
    const World& world,
    const SelectionModel* editorSelection,
    FrameSceneData& outFrame,
    TaskScheduler& scheduler,
    bool useParallel
) const {
    outFrame.clear();
    std::vector<int> lightIndexByEntity(world.lightRuntime_.size(), -1);
    const auto renderableVisible = [&](EntityId entity) {
        const VisibilityComponent* visibility = world.visibilities.tryGet(entity);
        return visibility != nullptr && visibility->visible;
    };
    const auto fillFrameRenderable = [&](EntityId entity, FrameRenderable& frameRenderable) {
        const RenderableComponent& renderable = world.renderables.get(entity);
        const MaterialComponent* material = world.materials.tryGet(entity);
        const TransformCacheEntry* cache = entity.index < world.transformCache_.size()
            ? &world.transformCache_[entity.index]
            : nullptr;

        frameRenderable = FrameRenderable{};
        frameRenderable.entity = entity;
        frameRenderable.mesh = renderable.mesh;
        frameRenderable.material = material != nullptr ? material->material : nullptr;
        frameRenderable.layer = renderable.layer;
        frameRenderable.visible = renderableVisible(entity);
        frameRenderable.localBounds = world.bounds.contains(entity)
            ? world.bounds.get(entity).localBounds
            : render::Bounds3{};
        if (cache != nullptr) {
            frameRenderable.modelMatrix = cache->worldMatrix;
            frameRenderable.hasWorldBounds = cache->hasWorldBounds;
            if (cache->hasWorldBounds) {
                frameRenderable.worldBounds = cache->worldBounds;
            }
        }
    };
    const auto appendLightVolumes = [&]() {
        for (EntityId entity : world.lightVolumes.entities()) {
            const LightVolumeComponent& volume = world.lightVolumes.get(entity);
            FrameLightVolume frameVolume{};
            frameVolume.entity = entity;
            const render::Bounds3 worldBounds = transformBounds(
                centeredBounds(glm::vec3(0.0f), glm::max(volume.halfExtents, glm::vec3(0.01f))),
                worldMatrixForEntity(world, entity)
            );
            frameVolume.minCorner = worldBounds.min;
            frameVolume.maxCorner = worldBounds.max;
            for (std::size_t lightIndex = 0; lightIndex < outFrame.lights.size(); ++lightIndex) {
                const FrameLight& light = outFrame.lights[lightIndex];
                if (!intersectsSphere(worldBounds, light.position, light.radius)) {
                    continue;
                }
                if (light.isMovable) {
                    frameVolume.movableLightIndices.push_back(static_cast<int>(lightIndex));
                } else {
                    frameVolume.staticLightIndices.push_back(static_cast<int>(lightIndex));
                }
            }
            outFrame.lightVolumes.push_back(std::move(frameVolume));
        }
    };
    const auto appendColliderDebug = [&]() {
        for (EntityId entity : world.boxColliders.entities()) {
            const BoxColliderComponent& collider = world.boxColliders.get(entity);
            if (!collider.showDebug || !renderableVisible(entity)) {
                continue;
            }

            FrameColliderDebug frameCollider{};
            frameCollider.entity = entity;
            frameCollider.shape = FrameColliderShape::Box;
            const OrientedBox box = makeOrientedBox(worldMatrixForEntity(world, entity), collider);
            frameCollider.bounds = box.aabb;
            frameCollider.modelMatrix = box.modelMatrix;
            outFrame.colliderDebug.push_back(std::move(frameCollider));
        }
        for (EntityId entity : world.sphereColliders.entities()) {
            const SphereColliderComponent& collider = world.sphereColliders.get(entity);
            if (!collider.showDebug || !renderableVisible(entity)) {
                continue;
            }

            const glm::mat4 modelMatrix = worldMatrixForEntity(world, entity);
            FrameColliderDebug frameCollider{};
            frameCollider.entity = entity;
            frameCollider.shape = FrameColliderShape::Sphere;
            frameCollider.center = glm::vec3(modelMatrix * glm::vec4(collider.center, 1.0f));
            frameCollider.radius = collider.radius * std::max(maxAxisScale(modelMatrix), 0.001f);
            frameCollider.bounds = centeredBounds(frameCollider.center, glm::vec3(frameCollider.radius));
            outFrame.colliderDebug.push_back(std::move(frameCollider));
        }
    };
    const auto appendGroundIndicators = [&]() {
        outFrame.groundIndicators.reserve(world.characters.size());
        for (EntityId entity : world.characters.entities()) {
            const VisibilityComponent* visibility = world.visibilities.tryGet(entity);
            if (visibility == nullptr || !visibility->visible ||
                entity.index >= world.transformCache_.size()) {
                continue;
            }

            const TransformCacheEntry& cache = world.transformCache_[entity.index];
            if (!cache.valid) {
                continue;
            }

            const CharacterComponent& character = world.characters.get(entity);
            glm::vec3 center(cache.worldMatrix[3]);
            center.y += 0.02f;
            outFrame.groundIndicators.push_back(FrameGroundIndicator{
                entity,
                center,
                std::max(character.groundIndicatorRadius, 0.01f),
                characterGroundIndicatorColor(character.affiliation)
            });
        }
    };

    if (!useParallel) {
        outFrame.reserve(
            world.renderables.size(),
            world.pointLights.size() + world.spotLights.size(),
            world.lightVolumes.size(),
            world.boxColliders.size() + world.sphereColliders.size()
        );

        for (EntityId entity : world.renderables.entities()) {
            FrameRenderable frameRenderable{};
            fillFrameRenderable(entity, frameRenderable);
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
        appendLightVolumes();
        appendColliderDebug();

    } else {
        outFrame.renderables.resize(world.renderables.size());
        outFrame.lights.resize(world.pointLights.size() + world.spotLights.size());

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
                    FrameRenderable frameRenderable{};
                    fillFrameRenderable(entity, frameRenderable);
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
        appendLightVolumes();
        appendColliderDebug();
    }

    appendGroundIndicators();

    for (FrameRenderable& renderable : outFrame.renderables) {
        render_extraction_detail::applyAnimatedRenderableState(
            world,
            renderable.entity,
            renderable,
            outFrame
        );
    }

    if (editorSelection == nullptr || !editorSelection->current().has_value()) {
        return;
    }

    const SelectionTarget selectedTarget = *editorSelection->current();
    const EntityId selected = selectedTarget.entity;
    outFrame.selection.entity = selected;
    outFrame.selection.component = selectedTarget.component;
    outFrame.selection.isLight = world.pointLights.contains(selected) || world.spotLights.contains(selected);
    if (selected.index < world.transformCache_.size()) {
        outFrame.selection.worldBounds = world.transformCache_[selected.index].worldBounds;
        outFrame.selection.hasWorldBounds = world.transformCache_[selected.index].hasWorldBounds;
    }

    const EntityId transformEntity = world.editableTransformEntity(selected);
    if (transformEntity.valid() && transformEntity.index < world.transformCache_.size()) {
        outFrame.selection.transformMatrix = world.transformCache_[transformEntity.index].worldMatrix;
    }
    for (const FrameRenderable& renderable : outFrame.renderables) {
        if (renderable.entity != selected) {
            continue;
        }
        outFrame.selection.worldBounds = renderable.worldBounds;
        outFrame.selection.hasWorldBounds = renderable.hasWorldBounds;
        break;
    }
    if (!outFrame.selection.hasWorldBounds && world.lightVolumes.contains(selected)) {
        const LightVolumeComponent& volume = world.lightVolumes.get(selected);
        outFrame.selection.worldBounds = transformBounds(
            centeredBounds(glm::vec3(0.0f), glm::max(volume.halfExtents, glm::vec3(0.01f))),
            worldMatrixForEntity(world, selected)
        );
        outFrame.selection.hasWorldBounds = true;
    }
    if (!outFrame.selection.hasWorldBounds && world.boxColliders.contains(selected)) {
        const BoxColliderComponent& collider = world.boxColliders.get(selected);
        const OrientedBox box = makeOrientedBox(worldMatrixForEntity(world, selected), collider);
        outFrame.selection.worldBounds = box.aabb;
        outFrame.selection.boundsModelMatrix = box.modelMatrix;
        outFrame.selection.hasBoundsModelMatrix = true;
        outFrame.selection.hasWorldBounds = true;
    }
    if (!outFrame.selection.hasWorldBounds && world.sphereColliders.contains(selected)) {
        const SphereColliderComponent& collider = world.sphereColliders.get(selected);
        const glm::mat4 modelMatrix = worldMatrixForEntity(world, selected);
        const glm::vec3 center = glm::vec3(modelMatrix * glm::vec4(collider.center, 1.0f));
        const float radius = collider.radius * std::max(maxAxisScale(modelMatrix), 0.001f);
        outFrame.selection.worldBounds = centeredBounds(center, glm::vec3(radius));
        outFrame.selection.hasWorldBounds = true;
    }
    if (!outFrame.selection.hasWorldBounds ||
        (world.animatedModels.contains(selected) && !world.renderables.contains(selected))) {
        render::Bounds3 animatedBounds{};
        if (render_extraction_detail::resolveAnimatedSelectionBounds(
                world,
                selected,
                outFrame,
                animatedBounds)) {
            outFrame.selection.worldBounds = animatedBounds;
            outFrame.selection.hasWorldBounds = true;
        }
    }

    render_extraction_detail::populateSelectionSkeletonDebug(world, selected, outFrame);
}

}  // namespace core
