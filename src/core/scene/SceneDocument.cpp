#include "SceneDocument.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "SceneAsset.hpp"
#include "core/ecs/World.hpp"

namespace core {
namespace {

std::string* mutableObjectName(SceneBlueprint& blueprint, SceneObjectHandle handle) {
    switch (handle.type) {
        case SceneObjectType::Primitive: return &blueprint.primitives.at(handle.index).name;
        case SceneObjectType::Model: return &blueprint.models.at(handle.index).name;
        case SceneObjectType::DirectionalLight: return &blueprint.directionalLight->name;
        case SceneObjectType::LightVolume: return &blueprint.lightVolumes.at(handle.index).name;
        case SceneObjectType::PointLight: return &blueprint.pointLights.at(handle.index).name;
        case SceneObjectType::SpotLight: return &blueprint.spotLights.at(handle.index).name;
    }
    return nullptr;
}

TransformComponent* mutableObjectTransform(SceneBlueprint& blueprint, SceneObjectHandle handle) {
    switch (handle.type) {
        case SceneObjectType::Primitive: return &blueprint.primitives.at(handle.index).transform;
        case SceneObjectType::Model: return &blueprint.models.at(handle.index).transform;
        case SceneObjectType::LightVolume: return &blueprint.lightVolumes.at(handle.index).transform;
        case SceneObjectType::PointLight: return &blueprint.pointLights.at(handle.index).transform;
        case SceneObjectType::SpotLight: return &blueprint.spotLights.at(handle.index).transform;
        case SceneObjectType::DirectionalLight: return nullptr;
    }
    return nullptr;
}

}  // namespace

void SceneDocument::begin(
    SceneBlueprint blueprint,
    std::filesystem::path sourcePath,
    std::filesystem::path stagedPath
) {
    blueprint_ = std::move(blueprint);
    sourcePath_ = std::move(sourcePath);
    stagedPath_ = std::move(stagedPath);
    entityByObject_.clear();
    objectByEntity_.clear();
    rememberCanonical();
    migrationPending_ = blueprint_.sourceVersion < kSceneAssetVersion ||
        blueprint_.requiresExplicitObjectMigration;
    dirty_ = migrationPending_;
}

bool SceneDocument::load(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& stagedPath,
    std::string* error
) {
    SceneBlueprint blueprint{};
    if (!loadSceneAsset(sourcePath, blueprint, error)) {
        return false;
    }
    begin(std::move(blueprint), sourcePath, stagedPath);
    return true;
}

void SceneDocument::createEmpty(
    std::filesystem::path sourcePath,
    std::filesystem::path stagedPath
) {
    SceneBlueprint blueprint{};
    blueprint.sourceVersion = kSceneAssetVersion;
    begin(std::move(blueprint), std::move(sourcePath), std::move(stagedPath));
    dirty_ = true;
}

bool SceneDocument::save(std::string* error) {
    if (sourcePath_.empty()) {
        if (error != nullptr) {
            *error = "Use Save As before saving an untitled scene.";
        }
        return false;
    }
    return saveAs(sourcePath_, stagedPath_, error);
}

bool SceneDocument::saveAs(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& stagedPath,
    std::string* error
) {
    SceneBlueprint savedBlueprint = blueprint_;
    savedBlueprint.sourceVersion = kSceneAssetVersion;
    if (!saveSceneAsset(sourcePath, savedBlueprint, error)) {
        return false;
    }

    std::error_code equivalenceError{};
    const bool samePath = !stagedPath.empty() && std::filesystem::equivalent(
        sourcePath,
        stagedPath,
        equivalenceError
    );
    if (!stagedPath.empty() && (!samePath || equivalenceError)) {
        if (!saveSceneAsset(stagedPath, savedBlueprint, error)) {
            return false;
        }
    }

    SceneBlueprint persistedBlueprint{};
    if (!loadSceneAsset(sourcePath, persistedBlueprint, error)) {
        return false;
    }

    sourcePath_ = sourcePath;
    stagedPath_ = stagedPath;
    blueprint_ = std::move(persistedBlueprint);
    migrationPending_ = false;
    rememberCanonical();
    dirty_ = false;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void SceneDocument::replaceBlueprint(SceneBlueprint blueprint) {
    blueprint_ = std::move(blueprint);
    migrationPending_ = blueprint_.sourceVersion < kSceneAssetVersion ||
        blueprint_.requiresExplicitObjectMigration;
    entityByObject_.clear();
    objectByEntity_.clear();
    refreshDirty();
}

void SceneDocument::refreshDirty() {
    std::string canonical{};
    std::string ignoredError{};
    dirty_ = migrationPending_ ||
        !serializeSceneAsset(blueprint_, canonical, &ignoredError) ||
        canonical != savedCanonical_;
}

std::string SceneDocument::displayName() const {
    return sourcePath_.empty() ? "Untitled.scene" : sourcePath_.filename().string();
}

void SceneDocument::bindWorld(World& world) {
    entityByObject_.clear();
    objectByEntity_.clear();
    for (EntityId entity : world.authoredSceneObjects.entities()) {
        const AuthoredSceneObjectComponent& authored = world.authoredSceneObjects.get(entity);
        if (authored.id.empty()) {
            continue;
        }
        entityByObject_[authored.id] = entity;
        objectByEntity_[entity] = authored.id;
        world.parents.remove(entity);
    }

    for (const SceneObjectId& id : blueprint_.objectOrder) {
        const std::optional<SceneObjectId> parentId = objectParent(id);
        if (!parentId.has_value()) {
            continue;
        }
        const auto child = entityByObject_.find(id);
        const auto parent = entityByObject_.find(*parentId);
        if (child == entityByObject_.end() || parent == entityByObject_.end()) {
            continue;
        }
        world.parents.emplace(child->second, ParentComponent{parent->second});
        world.markTransformsDirty(child->second);
    }
}

std::optional<EntityId> SceneDocument::entityForObject(std::string_view id) const {
    const auto entity = entityByObject_.find(SceneObjectId(id));
    return entity == entityByObject_.end() ? std::nullopt : std::optional<EntityId>{entity->second};
}

std::optional<SceneObjectId> SceneDocument::objectForEntity(EntityId entity) const {
    const auto object = objectByEntity_.find(entity);
    return object == objectByEntity_.end() ? std::nullopt : std::optional<SceneObjectId>{object->second};
}

bool SceneDocument::isAuthoredEntity(EntityId entity) const {
    return objectByEntity_.contains(entity);
}

std::optional<SceneObjectHandle> SceneDocument::findObject(std::string_view id) const {
    for (std::size_t index = 0u; index < blueprint_.primitives.size(); ++index) {
        if (blueprint_.primitives[index].id == id) return SceneObjectHandle{SceneObjectType::Primitive, index};
    }
    for (std::size_t index = 0u; index < blueprint_.models.size(); ++index) {
        if (blueprint_.models[index].id == id) return SceneObjectHandle{SceneObjectType::Model, index};
    }
    if (blueprint_.directionalLight.has_value() && blueprint_.directionalLight->id == id) {
        return SceneObjectHandle{SceneObjectType::DirectionalLight, 0u};
    }
    for (std::size_t index = 0u; index < blueprint_.lightVolumes.size(); ++index) {
        if (blueprint_.lightVolumes[index].id == id) return SceneObjectHandle{SceneObjectType::LightVolume, index};
    }
    for (std::size_t index = 0u; index < blueprint_.pointLights.size(); ++index) {
        if (blueprint_.pointLights[index].id == id) return SceneObjectHandle{SceneObjectType::PointLight, index};
    }
    for (std::size_t index = 0u; index < blueprint_.spotLights.size(); ++index) {
        if (blueprint_.spotLights[index].id == id) return SceneObjectHandle{SceneObjectType::SpotLight, index};
    }
    return std::nullopt;
}

const std::string* SceneDocument::objectName(std::string_view id) const {
    const std::optional<SceneObjectHandle> handle = findObject(id);
    if (!handle.has_value()) return nullptr;
    return mutableObjectName(const_cast<SceneBlueprint&>(blueprint_), *handle);
}

const TransformComponent* SceneDocument::objectTransform(std::string_view id) const {
    const std::optional<SceneObjectHandle> handle = findObject(id);
    if (!handle.has_value()) return nullptr;
    return mutableObjectTransform(const_cast<SceneBlueprint&>(blueprint_), *handle);
}

std::optional<SceneObjectId> SceneDocument::objectParent(std::string_view id) const {
    const std::optional<SceneObjectHandle> handle = findObject(id);
    if (!handle.has_value()) return std::nullopt;
    switch (handle->type) {
        case SceneObjectType::Primitive: return blueprint_.primitives[handle->index].parentId;
        case SceneObjectType::Model: return blueprint_.models[handle->index].parentId;
        case SceneObjectType::LightVolume: return blueprint_.lightVolumes[handle->index].parentId;
        case SceneObjectType::PointLight: return blueprint_.pointLights[handle->index].parentId;
        case SceneObjectType::SpotLight: return blueprint_.spotLights[handle->index].parentId;
        case SceneObjectType::DirectionalLight: return std::nullopt;
    }
    return std::nullopt;
}

bool SceneDocument::objectCanHaveParent(std::string_view id) const {
    const std::optional<SceneObjectHandle> handle = findObject(id);
    return handle.has_value() && handle->type != SceneObjectType::DirectionalLight;
}

bool SceneDocument::setObjectTransform(std::string_view id, const TransformComponent& transform) {
    const std::optional<SceneObjectHandle> handle = findObject(id);
    if (!handle.has_value()) return false;
    TransformComponent* target = mutableObjectTransform(blueprint_, *handle);
    if (target == nullptr) return false;
    *target = transform;
    refreshDirty();
    return true;
}

bool SceneDocument::setObjectName(std::string_view id, std::string name) {
    const std::optional<SceneObjectHandle> handle = findObject(id);
    if (!handle.has_value() || name.empty()) return false;
    *mutableObjectName(blueprint_, *handle) = std::move(name);
    refreshDirty();
    return true;
}

void SceneDocument::rememberCanonical() {
    std::string ignoredError{};
    if (!serializeSceneAsset(blueprint_, savedCanonical_, &ignoredError)) {
        savedCanonical_.clear();
    }
}

}  // namespace core
