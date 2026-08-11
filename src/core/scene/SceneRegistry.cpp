#include "SceneRegistry.hpp"

#include <filesystem>
#include <string>
#include <string_view>

#include "SceneAsset.hpp"
#include "SceneModelFactory.hpp"

namespace core {

SceneBlueprint SceneRegistry::defaultScene(std::string* error) const {
    SceneBlueprint blueprint{};
    const std::string path = scene_detail::assetRootPath("scenes/DefaultScene.scene");
    if (!loadSceneAsset(path, blueprint, error)) {
        return {};
    }
    return blueprint;
}

SceneBlueprint SceneRegistry::deterministicTestScene(std::string* error) const {
    SceneBlueprint blueprint = defaultScene(error);
    if (error != nullptr && !error->empty()) {
        return {};
    }
    if (blueprint.models.size() < 3u) {
        if (error != nullptr) {
            *error = "DefaultScene.scene must contain the three deterministic character models.";
        }
        return {};
    }

    const auto primitiveById = [&](std::string_view id) -> ScenePrimitiveBlueprint* {
        for (ScenePrimitiveBlueprint& primitive : blueprint.primitives) {
            if (primitive.id == id) return &primitive;
        }
        return nullptr;
    };
    ScenePrimitiveBlueprint* ground = primitiveById("ground");
    ScenePrimitiveBlueprint* wallA = primitiveById("wall_a");
    ScenePrimitiveBlueprint* wallB = primitiveById("wall_b");
    if (ground == nullptr || wallA == nullptr || wallB == nullptr) {
        if (error != nullptr) {
            *error = "DefaultScene.scene must contain ground, wall_a and wall_b primitives.";
        }
        return {};
    }
    ground->transform.scale = glm::vec3(24.0f, 1.0f, 24.0f);
    wallA->transform.position = glm::vec3(-6.0f, 1.25f, 0.0f);
    wallA->transform.scale = glm::vec3(0.5f, 2.5f, 24.0f);
    wallB->transform.position = glm::vec3(6.0f, 1.25f, 0.0f);
    wallB->transform.scale = glm::vec3(0.5f, 2.5f, 24.0f);
    if (blueprint.models.size() > 3u) {
        blueprint.models.resize(3u);
    }
    blueprint.models[0].transform.position = glm::vec3(0.0f, 0.0f, -1.5f);
    blueprint.models[1].transform.position = glm::vec3(-2.0f, 0.0f, 1.5f);
    blueprint.models[2].transform.position = glm::vec3(2.0f, 0.0f, 1.5f);

    blueprint.lightVolumes = {
        LightVolumeBlueprint{
            "Test Light Volume",
            TransformComponent{},
            glm::vec3(20.0f)
        }
    };
    blueprint.pointLights = {
        PointLightBlueprint{
            "Test Point Light",
            TransformComponent{glm::vec3(0.0f, 4.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)},
            24.0f,
            glm::vec3(1.0f),
            8.0f,
            0.0f,
            false,
            true,
            0.0001f,
            0.002f
        }
    };
    blueprint.spotLights.clear();
    blueprint.lightVolumes.front().id = "test_light_volume";
    blueprint.pointLights.front().id = "test_point_light";
    blueprint.objectOrder.clear();
    for (const ScenePrimitiveBlueprint& primitive : blueprint.primitives) {
        blueprint.objectOrder.push_back(primitive.id);
    }
    for (const ModelInstanceBlueprint& model : blueprint.models) {
        blueprint.objectOrder.push_back(model.id);
    }
    if (blueprint.directionalLight.has_value()) {
        blueprint.objectOrder.push_back(blueprint.directionalLight->id);
    }
    blueprint.objectOrder.push_back(blueprint.lightVolumes.front().id);
    blueprint.objectOrder.push_back(blueprint.pointLights.front().id);
    if (error != nullptr) {
        error->clear();
    }
    return blueprint;
}

std::filesystem::path SceneRegistry::sourceSceneDirectory() const {
    return std::filesystem::path(ALKANZAR_SCENE_SOURCE_DIR);
}

std::filesystem::path SceneRegistry::stagedSceneDirectory() const {
    return std::filesystem::path(ALKANZAR_SCENE_STAGED_DIR);
}

std::filesystem::path SceneRegistry::defaultSourceScenePath() const {
    return sourceSceneDirectory() / "DefaultScene.scene";
}

std::filesystem::path SceneRegistry::defaultStagedScenePath() const {
    return stagedSceneDirectory() / "DefaultScene.scene";
}

}  // namespace core
