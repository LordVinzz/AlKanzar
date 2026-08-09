#include <array>
#include <cassert>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

std::string characterObjectLua(
    std::string_view variable,
    std::string_view controlFields
) {
    std::string lua{};
    lua += std::string(variable) + " = Create({ type = \"Model\", name = \"" +
        std::string(variable) + "\", asset = \"Hero.glb\", layer = \"Actors\" })\n";
    lua += std::string(variable) + ".character({\n";
    lua += "    affiliation = \"FriendlyNpc\",\n";
    lua += controlFields;
    lua += R"lua(    race = "Human",
    kit = "Fighter",
    abilities = {
        strength = 10, agility = 10, physique = 10,
        intelligence = 10, faith = 10, charisma = 10,
    },
    skills = {},
    vitals = { current_hp = 45, maximum_hp = 45, mana = 0 },
})
)lua";
    lua += "scene.add(" + std::string(variable) + ")\n";
    return lua;
}

std::string makeCharacterControlAsset(
    const std::vector<std::pair<std::string_view, std::string_view>>& characters
) {
    std::string lua = "\nscene = Create({ type = \"Scene\" })\n";
    for (const auto& [variable, controlFields] : characters) {
        lua += characterObjectLua(variable, controlFields);
    }
    lua += "scene.build()\n";
    return makeSceneAsset(lua);
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
sun = Create({
    type = "DirectionalLight",
    name = "Test Sun",
    direction = { x = 0.0, y = -2.0, z = 0.0 },
    color = { x = 1.0, y = 0.9, z = 0.8 },
    intensity = 2.5,
})
scene.add(sun)
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
    assert(core::isPlayerControlled(scene.models[0].character->controller));
    assert(scene.models[0].character->partyMember.has_value());
    assert(scene.models[0].character->partyMember->slot == 0u);
    assert(scene.models[0].character->skills.ranks[
        static_cast<std::size_t>(core::CharacterSkill::Perception)
    ] == core::SkillRank::Initiate);
    assert(scene.directionalLight.has_value());
    assert(scene.directionalLight->name == "Test Sun");
    assert(scene.directionalLight->direction == glm::vec3(0.0f, -1.0f, 0.0f));
    assert(scene.directionalLight->color == glm::vec3(1.0f, 0.9f, 0.8f));
    assert(scene.directionalLight->intensity == 2.5f);
}

void testSceneAllowsOnlyOneValidDirectionalLight() {
    core::SceneBlueprint scene{};
    std::string error{};
    assert(!core::parseSceneAsset(
        makeSceneAsset(R"lua(
scene = Create({ type = "Scene" })
first = Create({ type = "DirectionalLight", name = "First", direction = { x = 0, y = -1, z = 0 } })
second = Create({ type = "DirectionalLight", name = "Second", direction = { x = 1, y = -1, z = 0 } })
scene.add(first)
scene.add(second)
scene.build()
)lua"),
        scene,
        &error
    ));
    assert(error.find("only one DirectionalLight") != std::string::npos);

    assert(!core::parseSceneAsset(
        makeSceneAsset(R"lua(
scene = Create({ type = "Scene" })
sun = Create({ type = "DirectionalLight", name = "Sun", direction = { x = 0, y = 0, z = 0 } })
scene.add(sun)
scene.build()
)lua"),
        scene,
        &error
    ));
    assert(error.find("directional-light") != std::string::npos);
}

void testSceneCharacterControlFieldsAreStrictAndPartySlotsAreUnique() {
    core::SceneBlueprint scene{};
    std::string error{};
    assert(core::parseSceneAsset(
        makeCharacterControlAsset({
            {"companion", "    controller = \"Player\",\n    party_slot = 3,\n"}
        }),
        scene,
        &error
    ));
    assert(scene.models[0].character.has_value());
    assert(core::isPlayerControlled(scene.models[0].character->controller));
    assert(scene.models[0].character->partyMember->slot == 3u);

    assert(!core::parseSceneAsset(
        makeCharacterControlAsset({
            {"missing_slot", "    controller = \"Player\",\n"}
        }),
        scene,
        &error
    ));
    assert(error.find("party_slot") != std::string::npos);

    assert(!core::parseSceneAsset(
        makeCharacterControlAsset({
            {"unknown", "    controller = \"Remote\",\n"}
        }),
        scene,
        &error
    ));
    assert(error.find("controller") != std::string::npos);

    assert(!core::parseSceneAsset(
        makeCharacterControlAsset({
            {"first", "    controller = \"Player\",\n    party_slot = 1,\n"},
            {"second", "    controller = \"Player\",\n    party_slot = 1,\n"}
        }),
        scene,
        &error
    ));
    assert(error.find("same party_slot") != std::string::npos);
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
    assert(scene.models.size() == 6u);
    assert(scene.directionalLight.has_value());
    assert(scene.directionalLight->name == "Sun");
    assert(scene.directionalLight->color == glm::vec3(1.0f, 0.93f, 0.82f));
    assert(scene.directionalLight->intensity == 3.0f);
    assert(scene.lightVolumes.size() == 1u);
    assert(scene.pointLights.size() == 1u);
    assert(scene.spotLights.size() == 1u);
    assert(scene.navMeshAssetPath == "navmeshes/DefaultScene.navmesh");
    assert(scene.models[3].character.has_value());
    assert(scene.models[4].character.has_value());
    assert(core::isPlayerControlled(scene.models[3].character->controller));
    assert(core::isPlayerControlled(scene.models[4].character->controller));
    assert(scene.models[3].character->partyMember->slot == 1u);
    assert(scene.models[4].character->partyMember->slot == 2u);
    assert(scene.models[5].name == "House");
    assert(scene.models[5].fitToFootprint);
    assert(scene.models[5].footprint == 7.5f);
    assert(scene.models[5].transform.position == glm::vec3(-3.0f, 0.0f, -8.0f));
    assert(scene.pointLights[0].castsShadow);
    assert(scene.pointLights[0].radius == 40.0f);
    assert(scene.spotLights[0].target == glm::vec3(-3.0f, 1.2f, -8.0f));
}

}  // namespace

int main() {
    testSceneDslBuildsTypedBlueprint();
    testSceneAllowsOnlyOneValidDirectionalLight();
    testSceneCharacterControlFieldsAreStrictAndPartySlotsAreUnique();
    testSceneHeaderTypeAndVersionAreValidated();
    testSceneMustBuildAndAddEveryObject();
    testSceneDslIsRestrictedAndStrict();
    testDefaultSceneAssetLoadsFromStagedAssets();
    return 0;
}
