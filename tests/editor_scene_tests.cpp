#include <cassert>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/ext/matrix_transform.hpp>

#include "core/ecs/World.hpp"
#include "core/editor/EditorGizmoMath.hpp"
#include "core/scene/SceneAsset.hpp"
#include "core/scene/SceneDocument.hpp"
#include "core/transform/TransformMath.hpp"

namespace {

core::SceneDocument makeDocument() {
    core::SceneBlueprint blueprint{};
    core::ModelInstanceBlueprint parent{};
    parent.id = "parent";
    parent.name = "Parent";
    parent.path = "Parent.glb";
    parent.transform.position = glm::vec3(3.0f, 0.0f, -2.0f);
    parent.transform.rotationDeg.y = 35.0f;
    blueprint.models.push_back(parent);

    core::PointLightBlueprint child{};
    child.id = "child";
    child.name = "Child";
    child.transform.position = glm::vec3(1.0f, 2.0f, 0.0f);
    blueprint.pointLights.push_back(child);
    blueprint.objectOrder = {parent.id, child.id};

    core::SceneDocument document{};
    document.begin(std::move(blueprint));
    return document;
}

void testDocumentMutationsRemainSerializableAndDirty() {
    core::SceneDocument document = makeDocument();
    assert(!document.dirty());

    const std::optional<core::SceneObjectId> duplicate = document.duplicateObject("child");
    assert(duplicate.has_value());
    assert(document.findObject(*duplicate).has_value());
    assert(document.dirty());

    std::string error{};
    assert(document.reparentObject(*duplicate, core::SceneObjectId{"parent"}, true, &error));
    assert(document.objectParent(*duplicate) == std::optional<core::SceneObjectId>{"parent"});
    assert(!document.reparentObject("parent", *duplicate, true, &error));
    assert(error.find("cycle") != std::string::npos);

    assert(document.deleteObjectRecursive("parent"));
    assert(!document.findObject("parent").has_value());
    assert(!document.findObject(*duplicate).has_value());
    assert(document.findObject("child").has_value());

    std::string serialized{};
    assert(core::serializeSceneAsset(document.blueprint(), serialized, &error));
}

void testPrimitiveCreationDuplicationAndPersistence() {
    core::SceneDocument document{};
    document.createEmpty();
    const std::optional<core::SceneObjectId> plane = document.addDefaultObject(
        core::SceneObjectType::Primitive,
        {},
        core::ScenePrimitiveShape::Plane
    );
    assert(plane.has_value());
    const std::optional<core::SceneObjectHandle> handle = document.findObject(*plane);
    assert(handle.has_value());
    assert(handle->type == core::SceneObjectType::Primitive);
    assert(document.blueprint().primitives[handle->index].shape == core::ScenePrimitiveShape::Plane);
    assert(document.blueprint().primitives[handle->index].material == core::SceneMaterialPreset::Soil);
    assert(document.blueprint().primitives[handle->index].layer == render::RenderLayer::Ground);

    const std::optional<core::SceneObjectId> duplicate = document.duplicateObject(*plane);
    assert(duplicate.has_value());
    assert(document.blueprint().primitives.size() == 2u);
    std::string bytes{};
    std::string error{};
    assert(core::serializeSceneAsset(document.blueprint(), bytes, &error));
    core::SceneBlueprint parsed{};
    assert(core::parseSceneAsset(bytes, parsed, &error));
    assert(parsed.primitives.size() == 2u);
    assert(parsed.primitives[0].shape == core::ScenePrimitiveShape::Plane);
}

void testReparentPreservesWorldTransform() {
    core::SceneDocument document = makeDocument();
    const core::TransformComponent before = *document.objectTransform("child");
    const glm::mat4 beforeWorld = core::composeTransform(before);
    std::string error{};
    assert(document.reparentObject("child", core::SceneObjectId{"parent"}, true, &error));

    const glm::mat4 parentWorld = core::composeTransform(*document.objectTransform("parent"));
    const glm::mat4 afterWorld = parentWorld * core::composeTransform(*document.objectTransform("child"));
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            assert(glm::abs(beforeWorld[column][row] - afterWorld[column][row]) < 1.0e-4f);
        }
    }
}

void testWorldBindingsDistinguishAuthoredRootsFromGeneratedChildren() {
    core::SceneDocument document = makeDocument();
    core::World world{};
    const core::EntityId parent = world.createEntity();
    world.transforms.emplace(parent, core::TransformComponent{});
    world.authoredSceneObjects.emplace(parent, core::AuthoredSceneObjectComponent{"parent"});
    const core::EntityId child = world.createEntity();
    world.transforms.emplace(child, core::TransformComponent{});
    world.authoredSceneObjects.emplace(child, core::AuthoredSceneObjectComponent{"child"});
    const core::EntityId generatedSection = world.createEntity();
    world.parents.emplace(generatedSection, core::ParentComponent{parent});

    std::string error{};
    assert(document.reparentObject("child", core::SceneObjectId{"parent"}, false, &error));
    document.bindWorld(world);
    assert(document.entityForObject("parent") == std::optional<core::EntityId>{parent});
    assert(document.objectForEntity(child) == std::optional<core::SceneObjectId>{"child"});
    assert(document.isAuthoredEntity(parent));
    assert(!document.isAuthoredEntity(generatedSection));
    assert(world.authoredSceneOwnerEntity(generatedSection) == parent);
    assert(world.parents.get(child).parent == parent);
}

void testCombatantCapturePersistsOnlyAuthoredState() {
    core::SceneBlueprint blueprint{};
    core::ModelInstanceBlueprint actor{};
    actor.id = "actor";
    actor.name = "Actor";
    actor.path = "Actor.glb";
    actor.character = core::CharacterBlueprint{};
    blueprint.models.push_back(actor);
    blueprint.objectOrder.push_back(actor.id);

    core::SceneDocument document{};
    document.begin(std::move(blueprint));
    core::World world{};
    const core::EntityId entity = world.createEntity();
    world.authoredSceneObjects.emplace(
        entity,
        core::AuthoredSceneObjectComponent{"actor"}
    );
    world.characters.emplace(entity, core::CharacterComponent{});
    core::CombatantComponent combatant{};
    combatant.state = core::CombatState::Scripted;
    combatant.scriptPhase = 4u;
    combatant.observedState = core::CombatState::Combat;
    combatant.stateElapsedSeconds = 9.0f;
    world.combatants.emplace(entity, combatant);
    document.bindWorld(world);

    assert(document.captureRuntimeObject("actor", world));
    const core::CombatantComponent& captured =
        *document.blueprint().models[0].combatant;
    assert(captured.state == core::CombatState::Scripted);
    assert(captured.scriptPhase == 4u);
    assert(captured.observedState == captured.state);
    assert(captured.stateElapsedSeconds == 0.0f);
    assert(document.dirty());

    world.combatants.remove(entity);
    assert(document.captureRuntimeObject("actor", world));
    assert(!document.blueprint().models[0].combatant.has_value());
}

void testInvalidAtomicSaveDoesNotReplaceExistingFile() {
    const std::filesystem::path path =
        std::filesystem::current_path() / "editor-scene-atomic-witness.scene";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "sentinel";
    }

    core::SceneBlueprint invalid{};
    core::PointLightBlueprint first{};
    first.id = "duplicate";
    first.name = "First";
    core::PointLightBlueprint second = first;
    second.name = "Second";
    invalid.pointLights = {first, second};
    invalid.objectOrder = {"duplicate"};
    std::string error{};
    assert(!core::saveSceneAsset(path, invalid, &error));

    std::ifstream input(path, std::ios::binary);
    std::string contents{};
    input >> contents;
    assert(contents == "sentinel");
    std::error_code cleanupError{};
    std::filesystem::remove(path, cleanupError);
}

void testLegacyDocumentRemainsDirtyUntilSavedAsV2() {
    core::SceneBlueprint legacy = makeDocument().blueprint();
    legacy.sourceVersion = core::kLegacySceneAssetVersion;
    core::SceneDocument document{};
    document.begin(std::move(legacy));
    assert(document.dirty());

    const std::filesystem::path path =
        std::filesystem::current_path() / "editor-scene-migration-witness.scene";
    std::error_code cleanupError{};
    std::filesystem::remove(path, cleanupError);
    std::string error{};
    assert(document.saveAs(path, {}, &error));
    assert(!document.dirty());
    assert(document.blueprint().sourceVersion == core::kSceneAssetVersion);

    std::ifstream input(path, std::ios::binary);
    std::string header{};
    std::getline(input, header);
    assert(header == "V2SCN-----");
    std::filesystem::remove(path, cleanupError);
}

void testEditedDocumentSavesMirrorsAndReopensCanonically() {
    core::SceneDocument document = makeDocument();
    std::string error{};
    assert(document.reparentObject("child", core::SceneObjectId{"parent"}, true, &error));
    assert(document.setObjectName("child", "Edited Child"));

    const std::filesystem::path source =
        std::filesystem::current_path() / "editor-scene-roundtrip-source.scene";
    const std::filesystem::path staged =
        std::filesystem::current_path() / "editor-scene-roundtrip-staged.scene";
    std::error_code cleanupError{};
    std::filesystem::remove(source, cleanupError);
    std::filesystem::remove(staged, cleanupError);
    assert(document.saveAs(source, staged, &error));
    assert(!document.dirty());

    core::SceneDocument reopened{};
    assert(reopened.load(source, staged, &error));
    assert(reopened.objectParent("child") == std::optional<core::SceneObjectId>{"parent"});
    assert(*reopened.objectName("child") == "Edited Child");
    core::SceneBlueprint stagedBlueprint{};
    assert(core::loadSceneAsset(staged, stagedBlueprint, &error));

    std::string sourceCanonical{};
    std::string stagedCanonical{};
    assert(core::serializeSceneAsset(reopened.blueprint(), sourceCanonical, &error));
    assert(core::serializeSceneAsset(stagedBlueprint, stagedCanonical, &error));
    assert(sourceCanonical == stagedCanonical);
    std::filesystem::remove(source, cleanupError);
    std::filesystem::remove(staged, cleanupError);
}

void testGizmoWorldToLocalConversionAndScaleGuard() {
    const core::TransformComponent parent{
        glm::vec3(3.0f, 1.0f, -2.0f),
        glm::vec3(0.0f, 35.0f, 0.0f),
        glm::vec3(2.0f)
    };
    const core::TransformComponent local{
        glm::vec3(1.0f, 0.5f, -0.25f),
        glm::vec3(5.0f, 15.0f, 0.0f),
        glm::vec3(1.0f)
    };
    const glm::mat4 parentWorld = core::composeTransform(parent);
    const glm::mat4 world = parentWorld * core::composeTransform(local);
    core::TransformComponent recovered{};
    assert(core::editorLocalTransformFromWorld(world, parentWorld, recovered));
    const glm::mat4 recoveredWorld = parentWorld * core::composeTransform(recovered);
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            assert(glm::abs(world[column][row] - recoveredWorld[column][row]) < 1.0e-4f);
        }
    }
    assert(!core::editorParentHasNonUniformScale(parentWorld));
    const glm::mat4 nonUniform = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 1.0f));
    assert(core::editorParentHasNonUniformScale(nonUniform));
}

}  // namespace

int main() {
    testDocumentMutationsRemainSerializableAndDirty();
    testPrimitiveCreationDuplicationAndPersistence();
    testReparentPreservesWorldTransform();
    testWorldBindingsDistinguishAuthoredRootsFromGeneratedChildren();
    testCombatantCapturePersistsOnlyAuthoredState();
    testInvalidAtomicSaveDoesNotReplaceExistingFile();
    testLegacyDocumentRemainsDirtyUntilSavedAsV2();
    testEditedDocumentSavesMirrorsAndReopensCanonically();
    testGizmoWorldToLocalConversionAndScaleGuard();
    return 0;
}
