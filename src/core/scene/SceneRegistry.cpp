#include "SceneRegistry.hpp"

namespace core {

SceneBlueprint SceneRegistry::defaultScene() const {
    SceneBlueprint blueprint{};
    blueprint.navMeshAssetPath = "navmeshes/DefaultScene.navmesh";
    blueprint.models = {
        ModelInstanceBlueprint{
            "Character",
            "Adventurer.glb",
            render::RenderLayer::Actors,
            TransformComponent{},
            false,
            0.0f
        },
        ModelInstanceBlueprint{
            "House",
            "FantasyHouse.glb",
            render::RenderLayer::Geometry,
            TransformComponent{
                glm::vec3(-3.0f, 0.0f, -8.0f),
                glm::vec3(0.0f, -35.0f, 0.0f),
                glm::vec3(1.0f)
            },
            true,
            7.5f
        }
    };

    blueprint.pointLights = {
        PointLightBlueprint{
            "Point Light",
            TransformComponent{glm::vec3(1.5f, 1.2f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)},
            40.0f,
            glm::vec3(0.9f, 0.7f, 1.0f),
            20.0f,
            0.0f,
            false,
            true,
            0.000015f,
            0.0045f
        }
    };

    blueprint.spotLights = {
        SpotLightBlueprint{
            "Spot Light",
            TransformComponent{glm::vec3(5.5f, 10.2f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)},
            32.0f,
            glm::vec3(0.55f, 0.70f, 0.95f),
            1.4f,
            glm::vec3(-3.0f, 1.2f, -8.0f),
            15.0f,
            25.0f,
            0.0f,
            false,
            true,
            0.0012f,
            0.004f
        }
    };

    return blueprint;
}

}  // namespace core
