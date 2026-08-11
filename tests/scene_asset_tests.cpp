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
    assert(scene.sourceVersion == 1u);
    assert(scene.requiresExplicitObjectMigration);
    assert(scene.primitives.size() == 5u);
    assert(scene.primitives[0].id == "ground");
    assert(scene.primitives[0].shape == core::ScenePrimitiveShape::Plane);
    assert(scene.primitives[0].material == core::SceneMaterialPreset::Soil);
    assert(scene.primitives[0].layer == render::RenderLayer::Ground);
    assert(scene.primitives[0].transform.scale == glm::vec3(84.0f, 1.0f, 84.0f));
    assert(scene.models[0].id == "model_001");
    assert(scene.objectOrder == std::vector<core::SceneObjectId>({
        "ground",
        "wall_a",
        "wall_b",
        "frustum_test_box",
        "occlusion_test_box",
        "model_001",
        "directional_light_001",
    }));
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
    assert(!core::parseSceneAsset(makeSceneAsset(payload, 3u, "SCN"), scene, &error));
    assert(error == "Unsupported scene version 3.");
}

void testSceneV2RequiresStableIdsAndValidHierarchy() {
    core::SceneBlueprint scene{};
    std::string error{};
    const std::string valid = makeSceneAsset(R"lua(
scene = Create({ type = "Scene" })
parent = Create({ type = "Model", id = "parent", name = "Parent", asset = "Parent.glb" })
child = Create({ type = "PointLight", id = "child", name = "Child", radius = 2.0 })
child.parent(parent)
scene.add(parent)
scene.add(child)
scene.build()
)lua", 2u);
    assert(core::parseSceneAsset(valid, scene, &error));
    assert(scene.sourceVersion == 2u);
    assert(scene.models[0].id == "parent");
    assert(scene.pointLights[0].parentId == std::optional<core::SceneObjectId>{"parent"});

    assert(!core::parseSceneAsset(
        makeSceneAsset(
            "\nscene = Create({ type = \"Scene\" })\n"
            "model = Create({ type = \"Model\", name = \"Missing\", asset = \"x.glb\" })\n"
            "scene.add(model)\nscene.build()\n",
            2u
        ),
        scene,
        &error
    ));
    assert(error.find("id") != std::string::npos);

    assert(!core::parseSceneAsset(
        makeSceneAsset(R"lua(
scene = Create({ type = "Scene" })
a = Create({ type = "Model", id = "same", name = "A", asset = "a.glb" })
b = Create({ type = "Model", id = "same", name = "B", asset = "b.glb" })
scene.add(a)
scene.add(b)
scene.build()
)lua", 2u),
        scene,
        &error
    ));
    assert(error.find("duplicates") != std::string::npos);

    assert(!core::parseSceneAsset(
        makeSceneAsset(R"lua(
scene = Create({ type = "Scene" })
a = Create({ type = "Model", id = "a", name = "A", asset = "a.glb" })
b = Create({ type = "Model", id = "b", name = "B", asset = "b.glb" })
a.parent(b)
b.parent(a)
scene.add(a)
scene.add(b)
scene.build()
)lua", 2u),
        scene,
        &error
    ));
    assert(error.find("cycle") != std::string::npos);

    assert(!core::parseSceneAsset(
        makeSceneAsset(R"lua(
scene = Create({ type = "Scene" })
parent = Create({ type = "Model", id = "parent", name = "Parent", asset = "a.glb" })
child = Create({ type = "Model", id = "child", name = "Child", asset = "b.glb" })
child.parent({ parameters = { id = "parent" } })
scene.add(parent)
scene.add(child)
scene.build()
)lua", 2u),
        scene,
        &error
    ));
    assert(error.find("returned by Create") != std::string::npos);
}

void testSceneV2SerializationRoundTripsAllAuthoredData() {
    core::SceneBlueprint before{};
    before.navMeshAssetPath = "navmeshes/RoundTrip.navmesh";

    core::ScenePrimitiveBlueprint primitive{};
    primitive.id = "primitive-box";
    primitive.name = "Authored Box";
    primitive.shape = core::ScenePrimitiveShape::Box;
    primitive.material = core::SceneMaterialPreset::Wood;
    primitive.layer = render::RenderLayer::Geometry;
    primitive.transform.position = glm::vec3(8.0f, 4.0f, -2.0f);
    primitive.transform.rotationDeg = glm::vec3(10.0f, 20.0f, 30.0f);
    primitive.transform.scale = glm::vec3(2.0f, 3.0f, 4.0f);
    before.primitives.push_back(primitive);

    core::ModelInstanceBlueprint parent{};
    parent.id = "model-parent";
    parent.name = "Quoted \"name\"";
    parent.path = "Hero.glb";
    parent.layer = render::RenderLayer::Actors;
    parent.materialProfile = core::SceneModelMaterialProfile::House;
    parent.transform.position = glm::vec3(1.25f, 2.5f, -3.75f);
    parent.transform.rotationDeg = glm::vec3(12.5f, -45.0f, 90.0f);
    parent.transform.scale = glm::vec3(0.75f, 1.25f, 1.5f);
    parent.fitToFootprint = true;
    parent.footprint = 2.25f;
    parent.character = core::CharacterBlueprint{};
    parent.character->character.affiliation = core::CharacterAffiliation::FriendlyNpc;
    parent.character->character.race = core::CharacterRace::Dwarf;
    parent.character->character.kit = core::CharacterKit::Mage;
    parent.character->character.experience = 123456;
    parent.character->character.groundIndicatorRadius = 0.875f;
    parent.character->controller.kind = core::CharacterControllerKind::Player;
    parent.character->partyMember = core::PartyMemberComponent{2u, false};
    parent.character->abilities = {17, 12, 15, 19, 14, 11};
    parent.character->skills.ranks[static_cast<std::size_t>(core::CharacterSkill::Running)] =
        core::SkillRank::Master;
    parent.character->skills.ranks[static_cast<std::size_t>(core::CharacterSkill::Knowledge)] =
        core::SkillRank::Legendary;
    parent.character->vitals = {37, 52, 81};
    before.models.push_back(parent);

    core::DirectionalLightBlueprint sun{};
    sun.id = "sun";
    sun.name = "Round-trip Sun";
    sun.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    sun.color = glm::vec3(0.625f, 0.75f, 0.875f);
    sun.intensity = 4.125f;
    before.directionalLight = sun;

    core::LightVolumeBlueprint volume{};
    volume.id = "light-volume";
    volume.name = "Light Volume";
    volume.parentId = parent.id;
    volume.transform.position = glm::vec3(-4.0f, 5.5f, 6.25f);
    volume.transform.rotationDeg = glm::vec3(5.0f, 10.0f, 15.0f);
    volume.transform.scale = glm::vec3(2.0f, 2.5f, 3.0f);
    volume.halfExtents = glm::vec3(7.0f, 8.0f, 9.0f);
    before.lightVolumes.push_back(volume);

    core::PointLightBlueprint child{};
    child.id = "child-light";
    child.name = "Child";
    child.parentId = parent.id;
    child.transform.position = glm::vec3(0.5f, 1.5f, 2.5f);
    child.transform.rotationDeg = glm::vec3(20.0f, 30.0f, 40.0f);
    child.transform.scale = glm::vec3(1.25f, 1.5f, 1.75f);
    child.radius = 4.5f;
    child.color = glm::vec3(0.25f, 0.5f, 0.75f);
    child.intensity = 6.5f;
    child.phase = 0.375f;
    child.isMovable = true;
    child.castsShadow = true;
    child.shadowBiasMin = 0.00125f;
    child.shadowBiasSlope = 0.0125f;
    before.pointLights.push_back(child);

    core::SpotLightBlueprint spot{};
    spot.id = "spot-light";
    spot.name = "Spot";
    spot.parentId = volume.id;
    spot.transform.position = glm::vec3(-1.0f, 3.0f, 5.0f);
    spot.transform.rotationDeg = glm::vec3(-10.0f, 35.0f, 70.0f);
    spot.transform.scale = glm::vec3(0.5f, 0.75f, 1.0f);
    spot.radius = 11.5f;
    spot.color = glm::vec3(0.9f, 0.8f, 0.7f);
    spot.intensity = 2.75f;
    spot.target = glm::vec3(6.0f, 4.0f, 2.0f);
    spot.innerAngle = 17.5f;
    spot.outerAngle = 42.5f;
    spot.phase = 0.625f;
    spot.isMovable = true;
    spot.castsShadow = true;
    spot.shadowBiasMin = 0.0025f;
    spot.shadowBiasSlope = 0.025f;
    before.spotLights.push_back(spot);

    before.objectOrder = {
        child.id,
        spot.id,
        parent.id,
        sun.id,
        volume.id,
        primitive.id,
    };

    std::string bytes{};
    std::string error{};
    assert(core::serializeSceneAsset(before, bytes, &error));
    assert(bytes.substr(0u, core::kContentFileHeaderSize) == "V2SCN-----");
    assert(bytes.find("ground_half_extent") == std::string::npos);
    assert(bytes.find("wall_height") == std::string::npos);
    assert(bytes.find("child-light") < bytes.find("model-parent"));

    core::SceneBlueprint after{};
    assert(core::parseSceneAsset(bytes, after, &error));
    assert(after.sourceVersion == 2u);
    assert(!after.requiresExplicitObjectMigration);
    assert(after.navMeshAssetPath == before.navMeshAssetPath);
    assert(after.objectOrder == before.objectOrder);
    assert(after.primitives.size() == 1u);
    assert(after.primitives[0].id == primitive.id);
    assert(after.primitives[0].name == primitive.name);
    assert(after.primitives[0].shape == primitive.shape);
    assert(after.primitives[0].material == primitive.material);
    assert(after.primitives[0].layer == primitive.layer);
    assert(after.primitives[0].transform.position == primitive.transform.position);
    assert(after.primitives[0].transform.rotationDeg == primitive.transform.rotationDeg);
    assert(after.primitives[0].transform.scale == primitive.transform.scale);
    assert(after.models.size() == 1u);
    assert(after.models[0].id == parent.id);
    assert(after.models[0].name == parent.name);
    assert(after.models[0].path == parent.path);
    assert(after.models[0].layer == parent.layer);
    assert(after.models[0].materialProfile == parent.materialProfile);
    assert(after.models[0].transform.position == parent.transform.position);
    assert(after.models[0].transform.rotationDeg == parent.transform.rotationDeg);
    assert(after.models[0].transform.scale == parent.transform.scale);
    assert(after.models[0].fitToFootprint == parent.fitToFootprint);
    assert(after.models[0].footprint == parent.footprint);
    assert(after.models[0].character.has_value());
    assert(after.models[0].character->character == parent.character->character);
    assert(after.models[0].character->controller == parent.character->controller);
    assert(after.models[0].character->abilities == parent.character->abilities);
    assert(after.models[0].character->skills == parent.character->skills);
    assert(after.models[0].character->vitals == parent.character->vitals);
    assert(after.models[0].character->skills.ranks[
        static_cast<std::size_t>(core::CharacterSkill::Running)
    ] == core::SkillRank::Master);
    assert(after.models[0].character->partyMember.has_value());
    assert(after.models[0].character->partyMember->slot == 2u);
    assert(!after.models[0].character->partyMember->active);
    assert(after.directionalLight.has_value());
    assert(after.directionalLight->id == sun.id);
    assert(after.directionalLight->name == sun.name);
    assert(after.directionalLight->direction == sun.direction);
    assert(after.directionalLight->color == sun.color);
    assert(after.directionalLight->intensity == sun.intensity);
    assert(after.lightVolumes.size() == 1u);
    assert(after.lightVolumes[0].id == volume.id);
    assert(after.lightVolumes[0].name == volume.name);
    assert(after.lightVolumes[0].parentId == volume.parentId);
    assert(after.lightVolumes[0].transform.position == volume.transform.position);
    assert(after.lightVolumes[0].transform.rotationDeg == volume.transform.rotationDeg);
    assert(after.lightVolumes[0].transform.scale == volume.transform.scale);
    assert(after.lightVolumes[0].halfExtents == volume.halfExtents);
    assert(after.pointLights.size() == 1u);
    assert(after.pointLights[0].id == child.id);
    assert(after.pointLights[0].name == child.name);
    assert(after.pointLights[0].parentId == child.parentId);
    assert(after.pointLights[0].transform.position == child.transform.position);
    assert(after.pointLights[0].transform.rotationDeg == child.transform.rotationDeg);
    assert(after.pointLights[0].transform.scale == child.transform.scale);
    assert(after.pointLights[0].radius == child.radius);
    assert(after.pointLights[0].color == child.color);
    assert(after.pointLights[0].intensity == child.intensity);
    assert(after.pointLights[0].phase == child.phase);
    assert(after.pointLights[0].isMovable == child.isMovable);
    assert(after.pointLights[0].castsShadow);
    assert(after.pointLights[0].shadowBiasMin == child.shadowBiasMin);
    assert(after.pointLights[0].shadowBiasSlope == child.shadowBiasSlope);
    assert(after.spotLights.size() == 1u);
    assert(after.spotLights[0].id == spot.id);
    assert(after.spotLights[0].name == spot.name);
    assert(after.spotLights[0].parentId == spot.parentId);
    assert(after.spotLights[0].transform.position == spot.transform.position);
    assert(after.spotLights[0].transform.rotationDeg == spot.transform.rotationDeg);
    assert(after.spotLights[0].transform.scale == spot.transform.scale);
    assert(after.spotLights[0].radius == spot.radius);
    assert(after.spotLights[0].color == spot.color);
    assert(after.spotLights[0].intensity == spot.intensity);
    assert(after.spotLights[0].target == spot.target);
    assert(after.spotLights[0].innerAngle == spot.innerAngle);
    assert(after.spotLights[0].outerAngle == spot.outerAngle);
    assert(after.spotLights[0].phase == spot.phase);
    assert(after.spotLights[0].isMovable == spot.isMovable);
    assert(after.spotLights[0].castsShadow == spot.castsShadow);
    assert(after.spotLights[0].shadowBiasMin == spot.shadowBiasMin);
    assert(after.spotLights[0].shadowBiasSlope == spot.shadowBiasSlope);
}

void testSceneV2PrimitiveValidationAndLegacyLayoutMigration() {
    core::SceneBlueprint scene{};
    std::string error{};
    assert(!core::parseSceneAsset(
        makeSceneAsset(R"lua(
scene = Create({ type = "Scene" })
primitive = Create({
    type = "Primitive",
    id = "bad-shape",
    name = "Bad Shape",
    shape = "Sphere",
    material = "Rock",
})
scene.add(primitive)
scene.build()
)lua", 2u),
        scene,
        &error
    ));
    assert(error.find("unknown primitive shape") != std::string::npos);

    assert(core::parseSceneAsset(
        makeSceneAsset(R"lua(
scene = Create({
    type = "Scene",
    ground_half_extent = 20.0,
    wall_height = 3.0,
    wall_offset = 4.0,
    wall_length = 6.0,
    wall_thickness = 0.75,
})
scene.build()
)lua", 2u),
        scene,
        &error
    ));
    assert(scene.requiresExplicitObjectMigration);
    assert(scene.primitives.size() == 5u);
    assert(scene.primitives[0].transform.scale == glm::vec3(40.0f, 1.0f, 40.0f));
    assert(scene.primitives[1].transform.position == glm::vec3(-4.0f, 1.5f, 0.0f));
    assert(scene.primitives[1].transform.scale == glm::vec3(0.75f, 3.0f, 12.0f));
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
    assert(scene.sourceVersion == 2u);
    assert(!scene.requiresExplicitObjectMigration);
    assert(scene.primitives.size() == 5u);
    assert(scene.primitives[0].id == "ground");
    assert(scene.primitives[0].shape == core::ScenePrimitiveShape::Plane);
    assert(scene.primitives[0].transform.scale == glm::vec3(1000.0f, 1.0f, 1000.0f));
    assert(scene.primitives[1].id == "wall_a");
    assert(scene.primitives[1].transform.position == glm::vec3(-3.0f, 1.25f, 0.0f));
    assert(scene.primitives[1].transform.scale == glm::vec3(0.5f, 2.5f, 10.0f));
    assert(scene.objectOrder.size() == 15u);
    assert(scene.models[0].id == "player");
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
    assert(scene.models[5].materialProfile == core::SceneModelMaterialProfile::House);
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
    testSceneV2RequiresStableIdsAndValidHierarchy();
    testSceneV2SerializationRoundTripsAllAuthoredData();
    testSceneV2PrimitiveValidationAndLegacyLayoutMigration();
    testSceneMustBuildAndAddEveryObject();
    testSceneDslIsRestrictedAndStrict();
    testDefaultSceneAssetLoadsFromStagedAssets();
    return 0;
}
