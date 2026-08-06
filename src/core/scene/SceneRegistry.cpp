#include "SceneRegistry.hpp"

#include <string>

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

    blueprint.groundHalfExtent = 12.0f;
    blueprint.wallHeight = 2.5f;
    blueprint.wallOffset = 6.0f;
    blueprint.wallLength = 12.0f;
    blueprint.wallThickness = 0.5f;
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
    if (error != nullptr) {
        error->clear();
    }
    return blueprint;
}

}  // namespace core
