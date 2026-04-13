#include "RenderExtractionSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>

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

bool computeSkinnedWorldBounds(
    const AnimatedModelComponent& animated,
    const SkinnedRenderableComponent& skinned,
    const glm::mat4& modelMatrix,
    render::Bounds3& outBounds
) {
    if (!animated.model ||
        skinned.sectionIndex < 0 ||
        skinned.sectionIndex >= static_cast<int>(animated.model->sections.size())) {
        return false;
    }

    const render::GltfMeshSection& section = animated.model->sections[static_cast<std::size_t>(skinned.sectionIndex)];
    const render::Mesh& mesh = section.mesh;
    if (mesh.positions.empty()) {
        return false;
    }

    const std::vector<glm::mat4>* skinMatrices =
        skinned.skinIndex >= 0 && skinned.skinIndex < static_cast<int>(animated.skinJointMatrices.size())
            ? &animated.skinJointMatrices[static_cast<std::size_t>(skinned.skinIndex)]
            : nullptr;

    bool hasBounds = false;
    for (std::size_t vertexIndex = 0; vertexIndex < mesh.positions.size(); ++vertexIndex) {
        glm::vec4 skinnedPosition(mesh.positions[vertexIndex], 1.0f);
        if (skinMatrices != nullptr &&
            !skinMatrices->empty() &&
            vertexIndex < mesh.jointIndices.size() &&
            vertexIndex < mesh.jointWeights.size()) {
            const glm::uvec4 joints = mesh.jointIndices[vertexIndex];
            const glm::vec4 weights = mesh.jointWeights[vertexIndex];
            const float totalWeight = weights.x + weights.y + weights.z + weights.w;
            if (totalWeight > 0.0f) {
                glm::mat4 skinMatrix(0.0f);
                if (weights.x > 0.0f && joints.x < skinMatrices->size()) {
                    skinMatrix += (*skinMatrices)[joints.x] * weights.x;
                }
                if (weights.y > 0.0f && joints.y < skinMatrices->size()) {
                    skinMatrix += (*skinMatrices)[joints.y] * weights.y;
                }
                if (weights.z > 0.0f && joints.z < skinMatrices->size()) {
                    skinMatrix += (*skinMatrices)[joints.z] * weights.z;
                }
                if (weights.w > 0.0f && joints.w < skinMatrices->size()) {
                    skinMatrix += (*skinMatrices)[joints.w] * weights.w;
                }
                skinnedPosition = skinMatrix * skinnedPosition;
            }
        }

        const glm::vec3 worldPosition = glm::vec3(modelMatrix * skinnedPosition);
        if (!hasBounds) {
            outBounds.min = worldPosition;
            outBounds.max = worldPosition;
            hasBounds = true;
            continue;
        }
        outBounds.min = glm::min(outBounds.min, worldPosition);
        outBounds.max = glm::max(outBounds.max, worldPosition);
    }

    return hasBounds;
}

bool computeConservativeSkinnedWorldBounds(
    const AnimatedModelComponent& animated,
    const SkinnedRenderableComponent& skinned,
    const glm::mat4& modelMatrix,
    render::Bounds3& outBounds
) {
    if (!animated.model ||
        skinned.sectionIndex < 0 ||
        skinned.sectionIndex >= static_cast<int>(animated.model->sections.size())) {
        return false;
    }

    const render::GltfMeshSection& section = animated.model->sections[static_cast<std::size_t>(skinned.sectionIndex)];
    if (section.jointInfluenceBounds.empty()) {
        return false;
    }

    const std::vector<glm::mat4>* skinMatrices =
        skinned.skinIndex >= 0 && skinned.skinIndex < static_cast<int>(animated.skinJointMatrices.size())
            ? &animated.skinJointMatrices[static_cast<std::size_t>(skinned.skinIndex)]
            : nullptr;
    if (skinMatrices == nullptr || skinMatrices->empty()) {
        return false;
    }

    bool hasBounds = false;
    for (const render::JointInfluenceBounds& jointBounds : section.jointInfluenceBounds) {
        if (jointBounds.jointIndex < 0 ||
            jointBounds.jointIndex >= static_cast<int>(skinMatrices->size())) {
            return false;
        }

        const render::Bounds3 transformedBounds = transformBounds(
            jointBounds.localBounds,
            modelMatrix * (*skinMatrices)[static_cast<std::size_t>(jointBounds.jointIndex)]
        );
        outBounds = hasBounds ? mergeBounds(outBounds, transformedBounds) : transformedBounds;
        hasBounds = true;
    }

    return hasBounds;
}

bool resolveAnimatedSelectionBounds(
    const World& world,
    EntityId selected,
    const FrameSceneData& frame,
    render::Bounds3& outBounds
) {
    if (!world.animatedModels.contains(selected)) {
        return false;
    }

    bool hasBounds = false;
    for (const FrameRenderable& renderable : frame.renderables) {
        if (world.animationOwnerEntity(renderable.entity) != selected) {
            continue;
        }
        outBounds = hasBounds ? mergeBounds(outBounds, renderable.worldBounds) : renderable.worldBounds;
        hasBounds = true;
    }

    return hasBounds;
}

void applyAnimatedRenderableState(
    const World& world,
    const EntityId entity,
    FrameRenderable& frameRenderable,
    FrameSceneData& outFrame
) {
    const SkinnedRenderableComponent* skinned = world.skinnedRenderables.tryGet(entity);
    if (skinned == nullptr || !world.isAlive(skinned->animationOwner)) {
        return;
    }

    const AnimatedModelComponent* animated = world.animatedModels.tryGet(skinned->animationOwner);
    if (animated == nullptr || !animated->model) {
        return;
    }

    if (skinned->animationOwner.index < world.transformCache_.size()) {
        // Skin palettes are generated in mesh-node space already, so skinned renderables
        // only need the owning scene transform here. Multiplying by the mesh node global
        // again double-applies exporter correction nodes such as unit-scale and axis fixes.
        frameRenderable.modelMatrix = world.transformCache_[skinned->animationOwner.index].worldMatrix;
    }

    const bool conservativeBounds =
        computeConservativeSkinnedWorldBounds(*animated, *skinned, frameRenderable.modelMatrix, frameRenderable.worldBounds);
    const bool exactBounds =
        !conservativeBounds && computeSkinnedWorldBounds(*animated, *skinned, frameRenderable.modelMatrix, frameRenderable.worldBounds);
    if (conservativeBounds || exactBounds) {
        frameRenderable.hasWorldBounds = true;
    } else if (entity.index >= world.transformCache_.size() || !world.transformCache_[entity.index].hasWorldBounds) {
        // Fall back to the raw section bounds when skin data is unavailable.
        frameRenderable.worldBounds = transformBounds(frameRenderable.localBounds, frameRenderable.modelMatrix);
        frameRenderable.hasWorldBounds = true;
    }

    if (skinned->skinIndex < 0 || skinned->skinIndex >= static_cast<int>(animated->skinJointMatrices.size())) {
        return;
    }

    const std::vector<glm::mat4>& skinMatrices = animated->skinJointMatrices[static_cast<std::size_t>(skinned->skinIndex)];
    if (skinMatrices.empty()) {
        return;
    }

    frameRenderable.skinned = true;
    frameRenderable.jointMatrixBase = static_cast<int>(outFrame.jointMatrices.size());
    frameRenderable.jointMatrixCount = static_cast<int>(skinMatrices.size());
    outFrame.jointMatrices.insert(outFrame.jointMatrices.end(), skinMatrices.begin(), skinMatrices.end());
}

void populateSelectionSkeletonDebug(
    const World& world,
    EntityId selected,
    FrameSceneData& outFrame
) {
    outFrame.selectionSkeleton.clear();

    const EntityId owner = world.animationOwnerEntity(selected);
    if (!owner.valid()) {
        return;
    }

    const AnimatedModelComponent* animated = world.animatedModels.tryGet(owner);
    if (animated == nullptr || !animated->model || animated->model->skins.empty()) {
        return;
    }

    int skinIndex = 0;
    if (const SkinnedRenderableComponent* skinned = world.skinnedRenderables.tryGet(selected);
        skinned != nullptr && skinned->animationOwner == owner) {
        skinIndex = skinned->skinIndex;
    }
    if (skinIndex < 0 || skinIndex >= static_cast<int>(animated->model->skins.size())) {
        return;
    }

    const render::SkinData& skin = animated->model->skins[static_cast<std::size_t>(skinIndex)];
    const std::size_t skeletonCount = std::min(
        skin.skeletonNodeIndices.size(),
        std::min(skin.skeletonParentIndices.size(), skin.skeletonJointIndices.size())
    );

    outFrame.selectionSkeleton.owner = owner;
    outFrame.selectionSkeleton.skinIndex = skinIndex;
    outFrame.selectionSkeleton.showOverlay = animated->showSkeletonOverlay;
    outFrame.selectionSkeleton.jointMatrices = skinIndex < static_cast<int>(animated->skinJointMatrices.size())
        ? animated->skinJointMatrices[static_cast<std::size_t>(skinIndex)]
        : std::vector<glm::mat4>{};

    if (skeletonCount == 0u) {
        return;
    }

    std::vector<std::vector<int>> childrenByParent(skeletonCount);
    for (std::size_t index = 0; index < skeletonCount; ++index) {
        const int parentIndex = skin.skeletonParentIndices[index];
        if (parentIndex >= 0 && parentIndex < static_cast<int>(skeletonCount)) {
            childrenByParent[static_cast<std::size_t>(parentIndex)].push_back(static_cast<int>(index));
        }
    }

    constexpr std::string_view kInvalidNodeName = "<invalid>";
    const auto nodeNameForDisplay = [&](int skeletonDisplayIndex) -> std::string_view {
        if (skeletonDisplayIndex < 0 || skeletonDisplayIndex >= static_cast<int>(skeletonCount)) {
            return kInvalidNodeName;
        }

        const int nodeIndex = skin.skeletonNodeIndices[static_cast<std::size_t>(skeletonDisplayIndex)];
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(animated->model->nodes.size())) {
            return kInvalidNodeName;
        }
        return animated->model->nodes[static_cast<std::size_t>(nodeIndex)].name;
    };

    std::vector<std::uint8_t> includeMask(skeletonCount, 0u);
    std::vector<std::uint8_t> resolvedMask(skeletonCount, 0u);
    const auto shouldInclude = [&](const auto& self, int skeletonDisplayIndex) -> bool {
        if (skeletonDisplayIndex < 0 || skeletonDisplayIndex >= static_cast<int>(skeletonCount)) {
            return false;
        }
        if (resolvedMask[static_cast<std::size_t>(skeletonDisplayIndex)] != 0u) {
            return includeMask[static_cast<std::size_t>(skeletonDisplayIndex)] != 0u;
        }

        bool hasIncludedChild = false;
        for (int childIndex : childrenByParent[static_cast<std::size_t>(skeletonDisplayIndex)]) {
            hasIncludedChild = self(self, childIndex) || hasIncludedChild;
        }

        const bool isJoint = skin.skeletonJointIndices[static_cast<std::size_t>(skeletonDisplayIndex)] >= 0;
        bool include = isJoint || hasIncludedChild;
        if (!include) {
            const int parentIndex = skin.skeletonParentIndices[static_cast<std::size_t>(skeletonDisplayIndex)];
            const bool parentIsJoint =
                parentIndex >= 0 &&
                parentIndex < static_cast<int>(skeletonCount) &&
                skin.skeletonJointIndices[static_cast<std::size_t>(parentIndex)] >= 0;
            include = parentIsJoint && endsWithIgnoreCase(nodeNameForDisplay(skeletonDisplayIndex), "_end");
        }

        resolvedMask[static_cast<std::size_t>(skeletonDisplayIndex)] = 1u;
        includeMask[static_cast<std::size_t>(skeletonDisplayIndex)] = include ? 1u : 0u;
        return include;
    };

    for (std::size_t index = 0; index < skeletonCount; ++index) {
        shouldInclude(shouldInclude, static_cast<int>(index));
    }

    const auto resolveNodeWorldPosition = [&](int nodeIndex) {
        glm::vec3 position(0.0f);
        if (nodeIndex >= 0 && nodeIndex < static_cast<int>(animated->globalNodeMatrices.size())) {
            position = glm::vec3(animated->globalNodeMatrices[static_cast<std::size_t>(nodeIndex)] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            if (owner.index < world.transformCache_.size()) {
                position = glm::vec3(world.transformCache_[owner.index].worldMatrix * glm::vec4(position, 1.0f));
            }
        }
        return position;
    };

    outFrame.selectionSkeleton.jointNames.reserve(skeletonCount);
    outFrame.selectionSkeleton.parentIndices.reserve(skeletonCount);
    outFrame.selectionSkeleton.jointWorldPositions.reserve(skeletonCount);

    const auto appendFilteredNode = [&](const auto& self, int skeletonDisplayIndex, int filteredParentIndex) -> void {
        if (skeletonDisplayIndex < 0 ||
            skeletonDisplayIndex >= static_cast<int>(skeletonCount) ||
            includeMask[static_cast<std::size_t>(skeletonDisplayIndex)] == 0u) {
            return;
        }

        const int filteredIndex = static_cast<int>(outFrame.selectionSkeleton.parentIndices.size());
        outFrame.selectionSkeleton.jointNames.emplace_back(nodeNameForDisplay(skeletonDisplayIndex));
        outFrame.selectionSkeleton.parentIndices.push_back(filteredParentIndex);
        outFrame.selectionSkeleton.jointWorldPositions.push_back(
            resolveNodeWorldPosition(skin.skeletonNodeIndices[static_cast<std::size_t>(skeletonDisplayIndex)])
        );

        for (int childIndex : childrenByParent[static_cast<std::size_t>(skeletonDisplayIndex)]) {
            self(self, childIndex, filteredIndex);
        }
    };

    for (std::size_t index = 0; index < skeletonCount; ++index) {
        if (skin.skeletonParentIndices[index] < 0) {
            appendFilteredNode(appendFilteredNode, static_cast<int>(index), -1);
        }
    }
}

}  // namespace

void RenderExtractionSystem::extract(
    const World& world,
    const SelectionModel& selection,
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

    for (FrameRenderable& renderable : outFrame.renderables) {
        applyAnimatedRenderableState(world, renderable.entity, renderable, outFrame);
    }

    if (!selection.current().has_value()) {
        return;
    }

    const SelectionTarget selectedTarget = *selection.current();
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
        if (resolveAnimatedSelectionBounds(world, selected, outFrame, animatedBounds)) {
            outFrame.selection.worldBounds = animatedBounds;
            outFrame.selection.hasWorldBounds = true;
        }
    }

    populateSelectionSkeletonDebug(world, selected, outFrame);
}

}  // namespace core
