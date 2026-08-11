#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "SceneBlueprint.hpp"
#include "core/ecs/Entity.hpp"

namespace core {

class World;

enum class SceneObjectType {
    Primitive = 0,
    Model,
    DirectionalLight,
    LightVolume,
    PointLight,
    SpotLight,
};

struct SceneObjectHandle {
    SceneObjectType type{SceneObjectType::Model};
    std::size_t index{0u};
};

class SceneDocument {
public:
    void begin(
        SceneBlueprint blueprint,
        std::filesystem::path sourcePath = {},
        std::filesystem::path stagedPath = {}
    );
    [[nodiscard]] bool load(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& stagedPath,
        std::string* error = nullptr
    );
    void createEmpty(
        std::filesystem::path sourcePath = {},
        std::filesystem::path stagedPath = {}
    );
    [[nodiscard]] bool save(std::string* error = nullptr);
    [[nodiscard]] bool saveAs(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& stagedPath,
        std::string* error = nullptr
    );

    [[nodiscard]] const SceneBlueprint& blueprint() const { return blueprint_; }
    [[nodiscard]] SceneBlueprint& blueprint() { return blueprint_; }
    void replaceBlueprint(SceneBlueprint blueprint);
    void refreshDirty();

    [[nodiscard]] bool dirty() const { return dirty_; }
    [[nodiscard]] bool hasPath() const { return !sourcePath_.empty(); }
    [[nodiscard]] const std::filesystem::path& sourcePath() const { return sourcePath_; }
    [[nodiscard]] const std::filesystem::path& stagedPath() const { return stagedPath_; }
    [[nodiscard]] std::string displayName() const;

    void bindWorld(World& world);
    [[nodiscard]] std::optional<EntityId> entityForObject(std::string_view id) const;
    [[nodiscard]] std::optional<SceneObjectId> objectForEntity(EntityId entity) const;
    [[nodiscard]] bool isAuthoredEntity(EntityId entity) const;

    [[nodiscard]] std::optional<SceneObjectHandle> findObject(std::string_view id) const;
    [[nodiscard]] const std::string* objectName(std::string_view id) const;
    [[nodiscard]] const TransformComponent* objectTransform(std::string_view id) const;
    [[nodiscard]] std::optional<SceneObjectId> objectParent(std::string_view id) const;
    [[nodiscard]] bool objectCanHaveParent(std::string_view id) const;

    [[nodiscard]] std::optional<SceneObjectId> addDefaultObject(
        SceneObjectType type,
        std::string_view modelAsset = "Adventurer.glb",
        ScenePrimitiveShape primitiveShape = ScenePrimitiveShape::Box
    );
    [[nodiscard]] std::optional<SceneObjectId> duplicateObject(std::string_view id);
    [[nodiscard]] bool deleteObjectRecursive(std::string_view id);
    [[nodiscard]] bool reparentObject(
        std::string_view id,
        std::optional<SceneObjectId> parentId,
        bool preserveWorldTransform,
        std::string* error = nullptr
    );
    [[nodiscard]] bool setObjectTransform(std::string_view id, const TransformComponent& transform);
    [[nodiscard]] bool setObjectName(std::string_view id, std::string name);
    [[nodiscard]] bool captureRuntimeObject(std::string_view id, const World& world);
    void captureRuntimeWorld(const World& world);

private:
    [[nodiscard]] SceneObjectId uniqueId(std::string_view prefix) const;
    [[nodiscard]] bool captureRuntimeObjectValues(std::string_view id, const World& world);
    void rememberCanonical();

    SceneBlueprint blueprint_{};
    std::filesystem::path sourcePath_{};
    std::filesystem::path stagedPath_{};
    std::string savedCanonical_{};
    bool dirty_{false};
    bool migrationPending_{false};
    std::unordered_map<SceneObjectId, EntityId> entityByObject_{};
    std::unordered_map<EntityId, SceneObjectId> objectByEntity_{};
};

}  // namespace core
