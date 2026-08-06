#include <array>
#include <cassert>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/content/ContentFileHeader.hpp"
#include "core/scene/SceneAsset.hpp"

namespace {

std::string makeSceneAsset(std::string_view lua, std::uint32_t version = 1u, std::string_view type = "SCN") {
    std::array<char, core::kContentFileHeaderSize> header{};
    std::string error{};
    assert(core::encodeTextContentFileHeader(version, type, header, &error));
    std::string asset(header.data(), header.size());
    asset.append(lua);
    return asset;
}

void testSceneDslBuildsTypedBlueprint() {
    const std::string asset = makeSceneAsset(R"lua(
scene = Create({
    type = "Scene",
    ground_half_extent = 42.0,
    navmesh = "navmeshes/Test.navmesh",
})
hero = Create({
    type = "Model",
    name = "Hero",
    asset = "Hero.glb",
    layer = "Actors",
})
hero.transform({
    position = { x = 1.0, y = 2.0, z = 3.0 },
    rotation = { x = 0.0, y = 90.0, z = 0.0 },
})
hero.character({
    affiliation = "Player",
    race = "Human",
    kit = "Fighter",
    experience = 0,
    indicator_radius = 0.7,
    abilities = {
        strength = 14,
        agility = 12,
        physique = 14,
        intelligence = 10,
        faith = 10,
        charisma = 10,
    },
    skills = { "Running", "Perception" },
    vitals = { current_hp = 53, maximum_hp = 53, mana = 0 },
})
scene.add(hero)
scene.build()
)lua");

    core::SceneBlueprint scene{};
    std::string error{};
    assert(core::parseSceneAsset(asset, scene, &error, "memory.scene"));
    assert(error.empty());
    assert(scene.groundHalfExtent == 42.0f);
    assert(scene.navMeshAssetPath == "navmeshes/Test.navmesh");
    assert(scene.models.size() == 1u);
    assert(scene.models[0].name == "Hero");
    assert(scene.models[0].layer == render::RenderLayer::Actors);
    assert(scene.models[0].transform.position == glm::vec3(1.0f, 2.0f, 3.0f));
    assert(scene.models[0].transform.rotationDeg == glm::vec3(0.0f, 90.0f, 0.0f));
    assert(scene.models[0].character.has_value());
    assert(scene.models[0].character->character.affiliation == core::CharacterAffiliation::Player);
    assert(scene.models[0].character->skills.ranks[
        static_cast<std::size_t>(core::CharacterSkill::Perception)
    ] == core::SkillRank::Initiate);
}

void testSceneHeaderTypeAndVersionAreValidated() {
    core::SceneBlueprint scene{};
    std::string error{};
    const std::string payload = "\nscene = Create({ type = \"Scene\" })\nscene.build()\n";

    assert(!core::parseSceneAsset(makeSceneAsset(payload, 1u, "NAV"), scene, &error));
    assert(error == "Expected content type SCN, got NAV.");
    assert(!core::parseSceneAsset(makeSceneAsset(payload, 2u, "SCN"), scene, &error));
    assert(error == "Unsupported scene version 2.");
}

void testSceneMustBuildAndAddEveryObject() {
    core::SceneBlueprint scene{};
    std::string error{};
    assert(!core::parseSceneAsset(
        makeSceneAsset("\nscene = Create({ type = \"Scene\" })\n"),
        scene,
        &error
    ));
    assert(error.find("scene.build()") != std::string::npos);

    assert(!core::parseSceneAsset(
        makeSceneAsset(
            "\nscene = Create({ type = \"Scene\" })\n"
            "orphan = Create({ type = \"Model\", name = \"Orphan\", asset = \"x.glb\" })\n"
            "scene.build()\n"
        ),
        scene,
        &error
    ));
    assert(error.find("scene.add") != std::string::npos);
}

void testSceneDslIsRestrictedAndStrict() {
    core::SceneBlueprint scene{};
    std::string error{};
    assert(!core::parseSceneAsset(
        makeSceneAsset("\nos.execute(\"echo forbidden\")\n"),
        scene,
        &error
    ));
    assert(error.find("os") != std::string::npos);

    assert(!core::parseSceneAsset(
        makeSceneAsset(
            "\nscene = Create({ type = \"Scene\", ground_half_extnt = 42 })\n"
            "scene.build()\n"
        ),
        scene,
        &error
    ));
    assert(error.find("ground_half_extnt") != std::string::npos);
    assert(error.find("not supported") != std::string::npos);

    assert(!core::parseSceneAsset(
        makeSceneAsset("\nwhile true do end\n"),
        scene,
        &error
    ));
    assert(error.find("instruction budget") != std::string::npos);

    assert(!core::parseSceneAsset(
        makeSceneAsset(
            "\nscene = Create({ type = \"Scene\" })\n"
            "model = Create({ type = \"Model\", name = \"Escape\", asset = \"../secret.glb\" })\n"
            "scene.add(model)\n"
            "scene.build()\n"
        ),
        scene,
        &error
    ));
    assert(error.find("portable relative path") != std::string::npos);
}

void testDefaultSceneAssetLoadsFromStagedAssets() {
    core::SceneBlueprint scene{};
    std::string error{};
    assert(core::loadSceneAsset(
        std::filesystem::path("scenes/DefaultScene.scene"),
        scene,
        &error
    ));
    assert(error.empty());
    assert(scene.models.size() == 4u);
    assert(scene.lightVolumes.size() == 1u);
    assert(scene.pointLights.size() == 1u);
    assert(scene.spotLights.size() == 1u);
    assert(scene.navMeshAssetPath == "navmeshes/DefaultScene.navmesh");
    assert(scene.models[3].name == "House");
    assert(scene.models[3].fitToFootprint);
    assert(scene.models[3].footprint == 7.5f);
    assert(scene.models[3].transform.position == glm::vec3(-3.0f, 0.0f, -8.0f));
    assert(scene.pointLights[0].castsShadow);
    assert(scene.pointLights[0].radius == 40.0f);
    assert(scene.spotLights[0].target == glm::vec3(-3.0f, 1.2f, -8.0f));
}

}  // namespace

int main() {
    testSceneDslBuildsTypedBlueprint();
    testSceneHeaderTypeAndVersionAreValidated();
    testSceneMustBuildAndAddEveryObject();
    testSceneDslIsRestrictedAndStrict();
    testDefaultSceneAssetLoadsFromStagedAssets();
    return 0;
}
