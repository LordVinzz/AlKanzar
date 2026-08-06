#include <array>
#include <cassert>
#include <cstdlib>

#include "core/app/AppMode.hpp"
#include "core/app/AppState.hpp"
#include "core/app/FrameData.hpp"
#include "core/editor/SelectionModel.hpp"
#include "core/ecs/World.hpp"
#include "core/scene/SceneRegistry.hpp"
#include "core/systems/PartySelectionModel.hpp"
#include "core/systems/RenderExtractionSystem.hpp"
#include "core/systems/TaskScheduler.hpp"

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
    assert(gameplay.acceptsTimeControls);
    assert(!gameplay.acceptsEditorInput);
    assert(!gameplay.usesEditorSelection);

    assert(editor.rendersEditorUi);
    assert(editor.acceptsEditorInput);
    assert(editor.usesEditorSelection);
    assert(editor.showsEditorOverlays);
    assert(editor.syncsNavigationDebug);
    assert(!editor.acceptsGameplayOrders);

    assert(testTool.rendersWorld);
    assert(testTool.usesDeterministicScene);
    assert(!testTool.rendersEditorUi);
    assert(!testTool.acceptsEditorInput);
    assert(!testTool.acceptsGameplayOrders);
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
    testRenderExtractionCannotLeakEditorSelectionIntoRuntimeModes();
    testDeterministicTestSceneHasAStableMinimalLayout();
    return EXIT_SUCCESS;
}
