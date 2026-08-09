#pragma once

#include <algorithm>
#include <vector>

#include <glm/mat4x4.hpp>

#include "ComponentStore.hpp"
#include "Components.hpp"
#include "EntityPool.hpp"
#include "core/events/Signal.hpp"

namespace core {

class World {
public:
    EntityId createEntity() {
        const EntityId entity = entities_.create();
        ensureCacheSize(entity.index + 1u);
        return entity;
    }

    void destroyEntity(EntityId entity) {
        if (!entities_.isAlive(entity)) {
            return;
        }

        names.remove(entity);
        transforms.remove(entity);
        parents.remove(entity);
        visibilities.remove(entity);
        bounds.remove(entity);
        renderables.remove(entity);
        materials.remove(entity);
        animatedModels.remove(entity);
        skinnedRenderables.remove(entity);
        navSources.remove(entity);
        navSourceGeometry.remove(entity);
        navAgents.remove(entity);
        locomotion.remove(entity);
        lightVolumes.remove(entity);
        boxColliders.remove(entity);
        sphereColliders.remove(entity);
        rigidbodies.remove(entity);
        directionalLights.remove(entity);
        pointLights.remove(entity);
        spotLights.remove(entity);
        characters.remove(entity);
        characterControllers.remove(entity);
        partyMembers.remove(entity);
        abilityScores.remove(entity);
        skillRanks.remove(entity);
        characterVitals.remove(entity);
        if (entity.index < transformCache_.size()) {
            transformCache_[entity.index] = TransformCacheEntry{};
        }
        if (entity.index < lightRuntime_.size()) {
            lightRuntime_[entity.index] = LightRuntime{};
        }
        entities_.destroy(entity);
        transformsDirty_ = true;
        lightsDirty_ = true;
    }

    [[nodiscard]] bool isAlive(EntityId entity) const {
        return entities_.isAlive(entity);
    }

    void clear() {
        entities_.clear();
        names.clear();
        transforms.clear();
        parents.clear();
        visibilities.clear();
        bounds.clear();
        renderables.clear();
        materials.clear();
        animatedModels.clear();
        skinnedRenderables.clear();
        navSources.clear();
        navSourceGeometry.clear();
        navAgents.clear();
        locomotion.clear();
        lightVolumes.clear();
        boxColliders.clear();
        sphereColliders.clear();
        rigidbodies.clear();
        directionalLights.clear();
        pointLights.clear();
        spotLights.clear();
        characters.clear();
        characterControllers.clear();
        partyMembers.clear();
        abilityScores.clear();
        skillRanks.clear();
        characterVitals.clear();
        transformCache_.clear();
        lightRuntime_.clear();
        transformsDirty_ = true;
        lightsDirty_ = true;
    }

    void markTransformsDirty(EntityId entity) {
        transformsDirty_ = true;
        transformChanged_.notify(entity);
    }

    void markLightsDirty(EntityId entity) {
        lightsDirty_ = true;
        lightChanged_.notify(entity);
    }

    [[nodiscard]] bool transformsDirty() const {
        return transformsDirty_;
    }

    void clearTransformDirty() {
        transformsDirty_ = false;
    }

    [[nodiscard]] bool lightsDirty() const {
        return lightsDirty_;
    }

    void clearLightDirty() {
        lightsDirty_ = false;
    }

    [[nodiscard]] EntityId rootAncestor(EntityId entity) const {
        EntityId current = entity;
        while (true) {
            const ParentComponent* parent = parents.tryGet(current);
            if (parent == nullptr || !isAlive(parent->parent)) {
                return current;
            }
            current = parent->parent;
        }
    }

    [[nodiscard]] TransformComponent* editableTransform(EntityId entity) {
        EntityId current = entity;
        while (current.valid()) {
            if (const SkinnedRenderableComponent* skinned = skinnedRenderables.tryGet(current);
                skinned != nullptr &&
                skinned->animationOwner.valid() &&
                skinned->animationOwner != current &&
                isAlive(skinned->animationOwner)) {
                current = skinned->animationOwner;
                continue;
            }
            if (TransformComponent* transform = transforms.tryGet(current)) {
                return transform;
            }
            const ParentComponent* parent = parents.tryGet(current);
            if (parent == nullptr || !isAlive(parent->parent)) {
                break;
            }
            current = parent->parent;
        }
        return nullptr;
    }

    [[nodiscard]] EntityId editableTransformEntity(EntityId entity) const {
        EntityId current = entity;
        while (current.valid()) {
            if (const SkinnedRenderableComponent* skinned = skinnedRenderables.tryGet(current);
                skinned != nullptr &&
                skinned->animationOwner.valid() &&
                skinned->animationOwner != current &&
                isAlive(skinned->animationOwner)) {
                current = skinned->animationOwner;
                continue;
            }
            if (transforms.contains(current)) {
                return current;
            }
            const ParentComponent* parent = parents.tryGet(current);
            if (parent == nullptr || !isAlive(parent->parent)) {
                break;
            }
            current = parent->parent;
        }
        return {};
    }

    [[nodiscard]] EntityId animationOwnerEntity(EntityId entity) const {
        EntityId current = entity;
        while (current.valid()) {
            if (animatedModels.contains(current)) {
                return current;
            }
            if (const SkinnedRenderableComponent* skinned = skinnedRenderables.tryGet(current);
                skinned != nullptr && isAlive(skinned->animationOwner)) {
                return skinned->animationOwner;
            }

            const ParentComponent* parent = parents.tryGet(current);
            if (parent == nullptr || !isAlive(parent->parent)) {
                break;
            }
            current = parent->parent;
        }
        return {};
    }

    [[nodiscard]] EntityId characterOwnerEntity(EntityId entity) const {
        EntityId current = entity;
        while (current.valid() && isAlive(current)) {
            if (characters.contains(current)) {
                return current;
            }
            if (const SkinnedRenderableComponent* skinned = skinnedRenderables.tryGet(current);
                skinned != nullptr &&
                skinned->animationOwner != current &&
                isAlive(skinned->animationOwner)) {
                current = skinned->animationOwner;
                continue;
            }
            const ParentComponent* parent = parents.tryGet(current);
            if (parent == nullptr || !isAlive(parent->parent)) {
                break;
            }
            current = parent->parent;
        }
        return {};
    }

    void ensureCacheSize(std::size_t size) {
        if (transformCache_.size() < size) {
            transformCache_.resize(size);
        }
        if (lightRuntime_.size() < size) {
            lightRuntime_.resize(size);
        }
    }

    ComponentStore<NameComponent> names{};
    ComponentStore<TransformComponent> transforms{};
    ComponentStore<ParentComponent> parents{};
    ComponentStore<VisibilityComponent> visibilities{};
    ComponentStore<BoundsComponent> bounds{};
    ComponentStore<RenderableComponent> renderables{};
    ComponentStore<MaterialComponent> materials{};
    ComponentStore<AnimatedModelComponent> animatedModels{};
    ComponentStore<SkinnedRenderableComponent> skinnedRenderables{};
    ComponentStore<NavSourceComponent> navSources{};
    ComponentStore<NavSourceGeometryComponent> navSourceGeometry{};
    ComponentStore<NavAgentComponent> navAgents{};
    ComponentStore<LocomotionComponent> locomotion{};
    ComponentStore<LightVolumeComponent> lightVolumes{};
    ComponentStore<BoxColliderComponent> boxColliders{};
    ComponentStore<SphereColliderComponent> sphereColliders{};
    ComponentStore<RigidbodyComponent> rigidbodies{};
    ComponentStore<DirectionalLightComponent> directionalLights{};
    ComponentStore<PointLightComponent> pointLights{};
    ComponentStore<SpotLightComponent> spotLights{};
    ComponentStore<CharacterComponent> characters{};
    ComponentStore<CharacterControllerComponent> characterControllers{};
    ComponentStore<PartyMemberComponent> partyMembers{};
    ComponentStore<AbilityScoresComponent> abilityScores{};
    ComponentStore<SkillRanksComponent> skillRanks{};
    ComponentStore<CharacterVitalsComponent> characterVitals{};
    std::vector<TransformCacheEntry> transformCache_{};
    std::vector<LightRuntime> lightRuntime_{};

    Signal<EntityId>& transformChanged() { return transformChanged_; }
    Signal<EntityId>& lightChanged() { return lightChanged_; }

private:
    EntityPool entities_{};
    Signal<EntityId> transformChanged_{};
    Signal<EntityId> lightChanged_{};
    bool transformsDirty_{true};
    bool lightsDirty_{true};
};

}  // namespace core
