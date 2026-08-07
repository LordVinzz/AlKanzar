#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#include "core/app/AppMode.hpp"
#include "core/app/AppState.hpp"
#include "core/app/FrameData.hpp"
#include "core/editor/SelectionModel.hpp"
#include "core/ecs/World.hpp"
#include "core/scene/SceneRegistry.hpp"
#include "core/systems/PartySelectionModel.hpp"
#include "core/systems/PartySelectionSystem.hpp"
#include "core/systems/RenderExtractionSystem.hpp"
#include "core/systems/TaskScheduler.hpp"
#include "render/engine/RenderSceneView.hpp"

namespace {

constexpr std::array<core::AppRuntimeSystem, 7> kRuntimeSystems{
    core::AppRuntimeSystem::StateUpdate,
    core::AppRuntimeSystem::Navigation,
    core::AppRuntimeSystem::Animation,
    core::AppRuntimeSystem::Physics,
    core::AppRuntimeSystem::Transforms,
    core::AppRuntimeSystem::Lighting,
    core::AppRuntimeSystem::RenderExtraction,
};

void testModeCapabilitiesAreExplicitAndIsolated() {
    const core::AppModeCapabilities& gameplay = core::appModeCapabilities(core::AppMode::Gameplay);
    const core::AppModeCapabilities& editor = core::appModeCapabilities(core::AppMode::Editor);
    const core::AppModeCapabilities& testTool = core::appModeCapabilities(core::AppMode::TestTool);
    const core::AppModeCapabilities& bootstrap = core::appModeCapabilities(core::AppMode::Bootstrap);
    const core::AppModeCapabilities& shutdown = core::appModeCapabilities(core::AppMode::Shutdown);

    for (const core::AppRuntimeSystem system : kRuntimeSystems) {
        assert(gameplay.runs(system));
        assert(editor.runs(system));
        assert(testTool.runs(system));
        assert(!bootstrap.runs(system));
        assert(!shutdown.runs(system));
    }

    assert(gameplay.acceptsGameplayOrders);
    assert(gameplay.acceptsPartySelection);
    assert(gameplay.acceptsTimeControls);
    assert(!gameplay.acceptsEditorInput);
    assert(!gameplay.usesEditorSelection);

    assert(editor.rendersEditorUi);
    assert(editor.acceptsEditorInput);
    assert(editor.usesEditorSelection);
    assert(editor.showsEditorOverlays);
    assert(editor.syncsNavigationDebug);
    assert(!editor.acceptsGameplayOrders);
    assert(!editor.acceptsPartySelection);

    assert(testTool.rendersWorld);
    assert(testTool.usesDeterministicScene);
    assert(!testTool.rendersEditorUi);
    assert(!testTool.acceptsEditorInput);
    assert(!testTool.acceptsGameplayOrders);
    assert(!testTool.acceptsPartySelection);
    assert(!testTool.acceptsTimeControls);
    assert(!testTool.acceptsCameraInput);
    assert(!testTool.usesEditorSelection);
}

void testModeTransitionsRestoreThePreviousRuntime() {
    core::AppModeSession session{};
    assert(session.current() == core::AppMode::Shutdown);
    assert(session.transitionTo(core::AppMode::Bootstrap));
    assert(!session.transitionTo(core::AppMode::Bootstrap));
    assert(session.editorToggleTarget() == core::AppMode::Bootstrap);

    assert(session.transitionTo(core::AppMode::TestTool));
    assert(session.editorToggleTarget() == core::AppMode::Editor);
    assert(session.transitionTo(session.editorToggleTarget()));
    assert(session.current() == core::AppMode::Editor);
    assert(session.editorToggleTarget() == core::AppMode::TestTool);
    assert(session.transitionTo(session.editorToggleTarget()));
    assert(session.current() == core::AppMode::TestTool);

    assert(session.transitionTo(core::AppMode::Gameplay));
    assert(session.transitionTo(session.editorToggleTarget()));
    assert(session.current() == core::AppMode::Editor);
    assert(session.editorToggleTarget() == core::AppMode::Gameplay);

    assert(core::normalizeStartupMode(core::AppMode::Editor) == core::AppMode::Editor);
    assert(core::normalizeStartupMode(core::AppMode::TestTool) == core::AppMode::TestTool);
    assert(core::normalizeStartupMode(core::AppMode::Bootstrap) == core::AppMode::Gameplay);
    assert(core::normalizeStartupMode(core::AppMode::Shutdown) == core::AppMode::Gameplay);

    const char* testArguments[] = {"AlKanzar", "--test-tool"};
    assert(core::startupModeFromArguments(2, testArguments) == core::AppMode::TestTool);
    const char* editorArguments[] = {"AlKanzar", "--editor"};
    assert(core::startupModeFromArguments(2, editorArguments) == core::AppMode::Editor);
    const char* defaultArguments[] = {"AlKanzar"};
    assert(core::startupModeFromArguments(1, defaultArguments) == core::AppMode::Gameplay);
}

void testEveryModeResolvesToItsOwnState() {
    core::AppStateCollection states{};
    assert(states.forMode(core::AppMode::Bootstrap).mode() == core::AppMode::Bootstrap);
    assert(states.forMode(core::AppMode::Gameplay).mode() == core::AppMode::Gameplay);
    assert(states.forMode(core::AppMode::Editor).mode() == core::AppMode::Editor);
    assert(states.forMode(core::AppMode::TestTool).mode() == core::AppMode::TestTool);
    assert(states.forMode(core::AppMode::Shutdown).mode() == core::AppMode::Shutdown);
}

void testEditorAndPartySelectionsAreIndependent() {
    core::SelectionModel editorSelection{};
    core::PartySelectionModel partySelection{};
    const core::EntityId editorEntity{3u, 1u};
    const core::EntityId playerEntity{7u, 2u};

    editorSelection.set(editorEntity);
    partySelection.setLeader(playerEntity);
    assert(editorSelection.current()->entity == editorEntity);
    assert(*partySelection.leader() == playerEntity);

    editorSelection.clear();
    assert(!editorSelection.current().has_value());
    assert(*partySelection.leader() == playerEntity);

    partySelection.clear();
    assert(!partySelection.leader().has_value());
}

void testPartySelectionModelTracksMultipleCharactersAndLeader() {
    core::PartySelectionModel selection{};
    const core::EntityId first{4u, 1u};
    const core::EntityId second{8u, 2u};

    selection.setSelection({first, second, first, core::EntityId{}});
    assert(selection.selected().size() == 2u);
    assert(selection.selected()[0] == first);
    assert(selection.selected()[1] == second);
    assert(*selection.leader() == first);
    assert(selection.contains(second));

    selection.setLeader(second);
    assert(selection.selected().size() == 2u);
    assert(selection.selected()[0] == second);
    assert(selection.selected()[1] == first);
    assert(*selection.leader() == second);

    std::vector<core::EntityId> oversized{};
    for (std::uint32_t index = 0u; index < 8u; ++index) {
        oversized.push_back(core::EntityId{20u + index, 1u});
    }
    selection.setSelection(oversized);
    assert(selection.selected().size() == core::kMaximumPartySize);
    assert(selection.selected().front() == oversized.front());
    assert(selection.selected().back() == oversized[core::kMaximumPartySize - 1u]);
}

void testPartySelectionRectangleSelectsOnlyOwnedCharacters() {
    assert(!core::isPartySelectionDrag(10, 10, 13, 14));
    assert(core::isPartySelectionDrag(10, 10, 16, 10));

    const core::ScreenSelectionRect normalized = core::makeScreenSelectionRect(
        250,
        90,
        -20,
        5,
        200,
        100
    );
    assert(normalized.minX == 0.0f);
    assert(normalized.minY == 5.0f);
    assert(normalized.maxX == 200.0f);
    assert(normalized.maxY == 90.0f);

    core::World world{};
    core::FrameSceneData frame{};
    const auto addCharacter = [&](
        core::CharacterAffiliation affiliation,
        const render::Bounds3& bounds,
        std::optional<core::PartyMemberComponent> partyMember = std::nullopt
    ) {
        const core::EntityId entity = world.createEntity();
        core::CharacterComponent character{};
        character.affiliation = affiliation;
        world.characters.emplace(entity, character);
        world.characterControllers.emplace(
            entity,
            core::CharacterControllerComponent{
                partyMember.has_value()
                    ? core::CharacterControllerKind::Player
                    : core::CharacterControllerKind::Uncontrolled
            }
        );
        if (partyMember.has_value()) {
            world.partyMembers.emplace(entity, *partyMember);
        }
        core::FrameRenderable renderable{};
        renderable.entity = entity;
        renderable.worldBounds = bounds;
        renderable.hasWorldBounds = true;
        frame.renderables.push_back(renderable);
        return entity;
    };

    const core::EntityId first = addCharacter(
        core::CharacterAffiliation::Player,
        render::Bounds3{glm::vec3(-0.9f, -0.2f, 0.0f), glm::vec3(-0.6f, 0.2f, 0.1f)},
        core::PartyMemberComponent{0u, true}
    );
    const core::EntityId second = addCharacter(
        core::CharacterAffiliation::FriendlyNpc,
        render::Bounds3{glm::vec3(0.5f, -0.2f, 0.0f), glm::vec3(0.8f, 0.2f, 0.1f)},
        core::PartyMemberComponent{1u, true}
    );
    addCharacter(
        core::CharacterAffiliation::FriendlyNpc,
        render::Bounds3{glm::vec3(-0.4f, -0.2f, 0.0f), glm::vec3(-0.1f, 0.2f, 0.1f)}
    );
    addCharacter(
        core::CharacterAffiliation::HostileNpc,
        render::Bounds3{glm::vec3(0.0f, -0.2f, 0.0f), glm::vec3(0.2f, 0.2f, 0.1f)}
    );
    addCharacter(
        core::CharacterAffiliation::Player,
        render::Bounds3{glm::vec3(-0.8f, -0.2f, 0.0f), glm::vec3(-0.7f, 0.2f, 0.1f)},
        core::PartyMemberComponent{2u, false}
    );

    render::CameraMatrices camera{};
    const core::PartySelectionSystem system{};
    const std::vector<core::EntityId> leftSelection = system.selectOwnedCharacters(
        world,
        frame,
        camera,
        200,
        100,
        core::ScreenSelectionRect{0.0f, 30.0f, 45.0f, 70.0f}
    );
    assert(leftSelection == std::vector<core::EntityId>{first});

    const std::vector<core::EntityId> fullSelection = system.selectOwnedCharacters(
        world,
        frame,
        camera,
        200,
        100,
        core::ScreenSelectionRect{0.0f, 0.0f, 200.0f, 100.0f}
    );
    assert((fullSelection == std::vector<core::EntityId>{first, second}));

    const std::vector<core::EntityId> emptySelection = system.selectOwnedCharacters(
        world,
        frame,
        camera,
        200,
        100,
        core::ScreenSelectionRect{105.0f, 0.0f, 125.0f, 20.0f}
    );
    assert(emptySelection.empty());

    core::PartySelectionModel selection{};
    selection.setSelection({first, second});
    world.destroyEntity(first);
    system.pruneInvalidSelection(world, selection);
    assert(selection.selected() == std::vector<core::EntityId>{second});
    world.partyMembers.get(second).active = false;
    system.pruneInvalidSelection(world, selection);
    assert(selection.selected().empty());
}

void testRenderSceneCarriesTheGreenSelectionMarquee() {
    core::FrameSceneData frame{};
    frame.partySelectionMarquee.visible = true;
    frame.partySelectionMarquee.min = glm::vec2(12.0f, 20.0f);
    frame.partySelectionMarquee.max = glm::vec2(80.0f, 75.0f);
    core::TaskScheduler scheduler{};
    const render::RenderSceneView scene = render::buildRenderSceneView(frame, {}, scheduler, false);

    assert(scene.partySelectionMarquee.visible);
    assert(scene.partySelectionMarquee.min == frame.partySelectionMarquee.min);
    assert(scene.partySelectionMarquee.max == frame.partySelectionMarquee.max);
    assert(scene.partySelectionMarquee.color.g > 0.9f);

    frame.clear();
    assert(!frame.partySelectionMarquee.visible);
}

void testRenderExtractionCannotLeakEditorSelectionIntoRuntimeModes() {
    core::World world{};
    const core::EntityId entity = world.createEntity();
    world.transforms.emplace(entity, core::TransformComponent{});

    core::SelectionModel editorSelection{};
    editorSelection.set(entity);
    core::FrameSceneData frame{};
    core::TaskScheduler scheduler{};
    core::RenderExtractionSystem extraction{};

    extraction.extract(world, nullptr, frame, scheduler, false);
    assert(!frame.selection.entity.has_value());

    extraction.extract(world, editorSelection, frame, scheduler, false);
    assert(frame.selection.entity.has_value());
    assert(*frame.selection.entity == entity);
}

void testDeterministicTestSceneHasAStableMinimalLayout() {
    const core::SceneRegistry registry{};
    const core::SceneBlueprint first = registry.deterministicTestScene();
    const core::SceneBlueprint second = registry.deterministicTestScene();

    assert(first.models.size() == 3u);
    assert(first.models.size() == second.models.size());
    assert(first.pointLights.size() == 1u);
    assert(first.spotLights.empty());
    assert(first.lightVolumes.size() == 1u);
    assert(first.navMeshAssetPath == "navmeshes/DefaultScene.navmesh");

    for (std::size_t index = 0; index < first.models.size(); ++index) {
        assert(first.models[index].name == second.models[index].name);
        assert(first.models[index].path == second.models[index].path);
        assert(first.models[index].character.has_value());
        assert(first.models[index].transform.position.x == second.models[index].transform.position.x);
        assert(first.models[index].transform.position.y == second.models[index].transform.position.y);
        assert(first.models[index].transform.position.z == second.models[index].transform.position.z);
    }

    assert(first.models[0].character->character.affiliation == core::CharacterAffiliation::Player);
    assert(first.models[1].character->character.affiliation == core::CharacterAffiliation::FriendlyNpc);
    assert(first.models[2].character->character.affiliation == core::CharacterAffiliation::HostileNpc);
}

}  // namespace

int main() {
    testModeCapabilitiesAreExplicitAndIsolated();
    testModeTransitionsRestoreThePreviousRuntime();
    testEveryModeResolvesToItsOwnState();
    testEditorAndPartySelectionsAreIndependent();
    testPartySelectionModelTracksMultipleCharactersAndLeader();
    testPartySelectionRectangleSelectsOnlyOwnedCharacters();
    testRenderSceneCarriesTheGreenSelectionMarquee();
    testRenderExtractionCannotLeakEditorSelectionIntoRuntimeModes();
    testDeterministicTestSceneHasAStableMinimalLayout();
    return EXIT_SUCCESS;
}
