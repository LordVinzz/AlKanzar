#include "SceneDocument.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include <glm/glm.hpp>

#include "core/transform/TransformMath.hpp"

namespace core {
namespace {

std::optional<SceneObjectId>* mutableParent(SceneBlueprint& blueprint, SceneObjectHandle handle) {
    switch (handle.type) {
        case SceneObjectType::Primitive: return &blueprint.primitives.at(handle.index).parentId;
        case SceneObjectType::Model: return &blueprint.models.at(handle.index).parentId;
        case SceneObjectType::LightVolume: return &blueprint.lightVolumes.at(handle.index).parentId;
        case SceneObjectType::PointLight: return &blueprint.pointLights.at(handle.index).parentId;
        case SceneObjectType::SpotLight: return &blueprint.spotLights.at(handle.index).parentId;
        case SceneObjectType::DirectionalLight: return nullptr;
    }
    return nullptr;
}

glm::mat4 authoredWorldMatrix(
    const SceneDocument& document,
    std::string_view id,
    std::unordered_set<SceneObjectId>& active
) {
    if (!active.insert(SceneObjectId(id)).second) {
        return glm::mat4(1.0f);
    }
    const TransformComponent* transform = document.objectTransform(id);
    glm::mat4 world = transform != nullptr ? composeTransform(*transform) : glm::mat4(1.0f);
    const std::optional<SceneObjectId> parent = document.objectParent(id);
    if (parent.has_value()) {
        world = authoredWorldMatrix(document, *parent, active) * world;
    }
    active.erase(SceneObjectId(id));
    return world;
}

glm::mat4 authoredWorldMatrix(const SceneDocument& document, std::string_view id) {
    std::unordered_set<SceneObjectId> active{};
    return authoredWorldMatrix(document, id, active);
}

}  // namespace

SceneObjectId SceneDocument::uniqueId(std::string_view prefix) const {
    SceneObjectId candidate(prefix);
    for (std::size_t suffix = 1u; findObject(candidate).has_value(); ++suffix) {
        candidate = std::string(prefix) + "_" + std::to_string(suffix);
    }
    return candidate;
}

std::optional<SceneObjectId> SceneDocument::addDefaultObject(
    SceneObjectType type,
    std::string_view modelAsset,
    ScenePrimitiveShape primitiveShape
) {
    SceneObjectId id{};
    switch (type) {
        case SceneObjectType::Primitive: {
            id = uniqueId(primitiveShape == ScenePrimitiveShape::Plane ? "plane" : "box");
            ScenePrimitiveBlueprint primitive{};
            primitive.id = id;
            primitive.name = primitiveShape == ScenePrimitiveShape::Plane ? "Plane" : "Box";
            primitive.shape = primitiveShape;
            primitive.material = primitiveShape == ScenePrimitiveShape::Plane
                ? SceneMaterialPreset::Soil
                : SceneMaterialPreset::Rock;
            primitive.layer = primitiveShape == ScenePrimitiveShape::Plane
                ? render::RenderLayer::Ground
                : render::RenderLayer::Geometry;
            if (primitiveShape == ScenePrimitiveShape::Plane) {
                primitive.transform.scale = glm::vec3(10.0f, 1.0f, 10.0f);
            }
            blueprint_.primitives.push_back(std::move(primitive));
            break;
        }
        case SceneObjectType::Model: {
            id = uniqueId("model");
            ModelInstanceBlueprint model{};
            model.id = id;
            model.name = modelAsset == "FantasyHouse.glb" ? "House" : "Model";
            model.path = std::string(modelAsset);
            model.materialProfile = modelAsset == "FantasyHouse.glb"
                ? SceneModelMaterialProfile::House
                : SceneModelMaterialProfile::Imported;
            model.layer = modelAsset == "Adventurer.glb"
                ? render::RenderLayer::Actors
                : render::RenderLayer::Geometry;
            blueprint_.models.push_back(std::move(model));
            break;
        }
        case SceneObjectType::DirectionalLight: {
            if (blueprint_.directionalLight.has_value()) return std::nullopt;
            id = uniqueId("sun");
            DirectionalLightBlueprint light{};
            light.id = id;
            light.name = "Sun";
            blueprint_.directionalLight = std::move(light);
            break;
        }
        case SceneObjectType::LightVolume: {
            id = uniqueId("light_volume");
            LightVolumeBlueprint volume{};
            volume.id = id;
            volume.name = "Light Volume";
            blueprint_.lightVolumes.push_back(std::move(volume));
            break;
        }
        case SceneObjectType::PointLight: {
            id = uniqueId("point_light");
            PointLightBlueprint light{};
            light.id = id;
            light.name = "Point Light";
            light.transform.position.y = 2.0f;
            blueprint_.pointLights.push_back(std::move(light));
            break;
        }
        case SceneObjectType::SpotLight: {
            id = uniqueId("spot_light");
            SpotLightBlueprint light{};
            light.id = id;
            light.name = "Spot Light";
            light.transform.position.y = 4.0f;
            blueprint_.spotLights.push_back(std::move(light));
            break;
        }
    }
    blueprint_.objectOrder.push_back(id);
    refreshDirty();
    return id;
}

std::optional<SceneObjectId> SceneDocument::duplicateObject(std::string_view id) {
    const std::optional<SceneObjectHandle> handle = findObject(id);
    if (!handle.has_value() || handle->type == SceneObjectType::DirectionalLight) {
        return std::nullopt;
    }

    SceneObjectId duplicateId{};
    switch (handle->type) {
        case SceneObjectType::Primitive: {
            ScenePrimitiveBlueprint copy = blueprint_.primitives[handle->index];
            copy.id = uniqueId(copy.id + "_copy");
            copy.name += " Copy";
            duplicateId = copy.id;
            blueprint_.primitives.push_back(std::move(copy));
            break;
        }
        case SceneObjectType::Model: {
            ModelInstanceBlueprint copy = blueprint_.models[handle->index];
            copy.id = uniqueId(copy.id + "_copy");
            copy.name += " Copy";
            duplicateId = copy.id;
            blueprint_.models.push_back(std::move(copy));
            break;
        }
        case SceneObjectType::LightVolume: {
            LightVolumeBlueprint copy = blueprint_.lightVolumes[handle->index];
            copy.id = uniqueId(copy.id + "_copy");
            copy.name += " Copy";
            duplicateId = copy.id;
            blueprint_.lightVolumes.push_back(std::move(copy));
            break;
        }
        case SceneObjectType::PointLight: {
            PointLightBlueprint copy = blueprint_.pointLights[handle->index];
            copy.id = uniqueId(copy.id + "_copy");
            copy.name += " Copy";
            duplicateId = copy.id;
            blueprint_.pointLights.push_back(std::move(copy));
            break;
        }
        case SceneObjectType::SpotLight: {
            SpotLightBlueprint copy = blueprint_.spotLights[handle->index];
            copy.id = uniqueId(copy.id + "_copy");
            copy.name += " Copy";
            duplicateId = copy.id;
            blueprint_.spotLights.push_back(std::move(copy));
            break;
        }
        case SceneObjectType::DirectionalLight: return std::nullopt;
    }

    const auto orderPosition = std::find(blueprint_.objectOrder.begin(), blueprint_.objectOrder.end(), id);
    blueprint_.objectOrder.insert(
        orderPosition == blueprint_.objectOrder.end() ? orderPosition : std::next(orderPosition),
        duplicateId
    );
    refreshDirty();
    return duplicateId;
}

bool SceneDocument::deleteObjectRecursive(std::string_view id) {
    if (!findObject(id).has_value()) return false;
    std::unordered_set<SceneObjectId> removed{SceneObjectId(id)};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const SceneObjectId& candidate : blueprint_.objectOrder) {
            const std::optional<SceneObjectId> parent = objectParent(candidate);
            if (parent.has_value() && removed.contains(*parent) && removed.insert(candidate).second) {
                changed = true;
            }
        }
    }

    std::erase_if(blueprint_.primitives, [&](const auto& value) { return removed.contains(value.id); });
    std::erase_if(blueprint_.models, [&](const auto& value) { return removed.contains(value.id); });
    std::erase_if(blueprint_.lightVolumes, [&](const auto& value) { return removed.contains(value.id); });
    std::erase_if(blueprint_.pointLights, [&](const auto& value) { return removed.contains(value.id); });
    std::erase_if(blueprint_.spotLights, [&](const auto& value) { return removed.contains(value.id); });
    if (blueprint_.directionalLight.has_value() && removed.contains(blueprint_.directionalLight->id)) {
        blueprint_.directionalLight.reset();
    }
    std::erase_if(blueprint_.objectOrder, [&](const auto& value) { return removed.contains(value); });
    refreshDirty();
    return true;
}

bool SceneDocument::reparentObject(
    std::string_view id,
    std::optional<SceneObjectId> parentId,
    bool preserveWorldTransform,
    std::string* error
) {
    const std::optional<SceneObjectHandle> handle = findObject(id);
    if (!handle.has_value() || !objectCanHaveParent(id)) {
        if (error != nullptr) *error = "Only authored transformable objects can be reparented.";
        return false;
    }
    if (parentId.has_value() && (!findObject(*parentId).has_value() || !objectCanHaveParent(*parentId))) {
        if (error != nullptr) *error = "The requested parent is not a transformable authored object.";
        return false;
    }
    for (std::optional<SceneObjectId> ancestor = parentId; ancestor.has_value(); ancestor = objectParent(*ancestor)) {
        if (*ancestor == id) {
            if (error != nullptr) *error = "Reparenting would create a hierarchy cycle.";
            return false;
        }
    }

    std::optional<SceneObjectId>* targetParent = mutableParent(blueprint_, *handle);
    TransformComponent* targetTransform = const_cast<TransformComponent*>(objectTransform(id));
    const std::optional<SceneObjectId> oldParent = *targetParent;
    const TransformComponent oldTransform = *targetTransform;
    const glm::mat4 oldWorld = authoredWorldMatrix(*this, id);
    *targetParent = std::move(parentId);

    if (preserveWorldTransform) {
        glm::mat4 local = oldWorld;
        if (targetParent->has_value()) {
            local = glm::inverse(authoredWorldMatrix(*this, **targetParent)) * oldWorld;
        }
        if (!decomposeTransform(local, *targetTransform)) {
            *targetParent = oldParent;
            *targetTransform = oldTransform;
            if (error != nullptr) {
                *error = "The parent transform would introduce unsupported shear.";
            }
            return false;
        }
    }
    refreshDirty();
    if (error != nullptr) error->clear();
    return true;
}

}  // namespace core
