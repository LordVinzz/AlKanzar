#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <imgui.h>

#include "core/animation/AnimationSystem.hpp"
#include "core/editor/CommandHistory.hpp"
#include "core/editor/EditorSession.hpp"
#include "core/editor/EditorSessionImGuiSettings.hpp"
#include "core/ecs/ComponentStore.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/lighting/LightSystem.hpp"
#include "core/lighting/MaterialLibrary.hpp"
#include "core/systems/PickingSystem.hpp"
#include "core/profiling/ProfilerService.hpp"
#include "core/systems/RenderExtractionSystem.hpp"
#include "core/editor/SelectionModel.hpp"
#include "core/systems/TaskScheduler.hpp"
#include "core/transform/TransformSystem.hpp"
#include "core/ecs/World.hpp"
#include "render/resources/Profiling.hpp"
#include "render/resources/StaticGltfModel.hpp"
#include "render/pipeline/RenderLightPipeline.hpp"
#include "render/engine/RenderSceneView.hpp"

namespace {

bool nearlyEqual(double lhs, double rhs, double epsilon = 0.001) {
    return std::abs(lhs - rhs) <= epsilon;
}

core::TaskScheduler makeScheduler(std::size_t workerCount = 2u) {
    return core::TaskScheduler(core::TaskSchedulerConfig{workerCount});
}

bool containsNonMainThreadScope(const std::vector<core::ProfilerScopeNode>& scopes) {
    for (const core::ProfilerScopeNode& scope : scopes) {
        if (scope.thread != "Main") {
            return true;
        }
        if (containsNonMainThreadScope(scope.children)) {
            return true;
        }
    }
    return false;
}

bool readVarint(std::string_view data, std::size_t& offset, std::uint64_t& value) {
    value = 0u;
    int shift = 0;
    while (offset < data.size() && shift < 64) {
        const std::uint8_t byte = static_cast<std::uint8_t>(data[offset++]);
        value |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0u) {
            return true;
        }
        shift += 7;
    }
    return false;
}

bool skipProtoField(std::string_view data, std::size_t& offset, std::uint64_t wireType) {
    std::uint64_t length = 0u;
    switch (wireType) {
        case 0u:
            return readVarint(data, offset, length);
        case 1u:
            if (offset + 8u > data.size()) {
                return false;
            }
            offset += 8u;
            return true;
        case 2u:
            if (!readVarint(data, offset, length) || offset + length > data.size()) {
                return false;
            }
            offset += static_cast<std::size_t>(length);
            return true;
        case 5u:
            if (offset + 4u > data.size()) {
                return false;
            }
            offset += 4u;
            return true;
        default:
            return false;
    }
}

std::vector<std::string_view> collectMessageFieldValues(std::string_view message, std::uint64_t fieldNumber) {
    std::vector<std::string_view> values{};
    std::size_t offset = 0u;
    while (offset < message.size()) {
        std::uint64_t key = 0u;
        assert(readVarint(message, offset, key));
        const std::uint64_t currentField = key >> 3u;
        const std::uint64_t wireType = key & 0x07u;
        if (wireType == 2u) {
            std::uint64_t length = 0u;
            assert(readVarint(message, offset, length));
            assert(offset + length <= message.size());
            if (currentField == fieldNumber) {
                values.push_back(message.substr(offset, static_cast<std::size_t>(length)));
            }
            offset += static_cast<std::size_t>(length);
        } else {
            assert(skipProtoField(message, offset, wireType));
        }
    }
    return values;
}

std::optional<std::uint64_t> firstVarintFieldValue(std::string_view message, std::uint64_t fieldNumber) {
    std::size_t offset = 0u;
    while (offset < message.size()) {
        std::uint64_t key = 0u;
        if (!readVarint(message, offset, key)) {
            return std::nullopt;
        }
        const std::uint64_t currentField = key >> 3u;
        const std::uint64_t wireType = key & 0x07u;
        if (wireType == 0u) {
            std::uint64_t value = 0u;
            if (!readVarint(message, offset, value)) {
                return std::nullopt;
            }
            if (currentField == fieldNumber) {
                return value;
            }
        } else if (!skipProtoField(message, offset, wireType)) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::string readFileToString(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.is_open());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::filesystem::path repositoryRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::filesystem::path assetPath(const std::string& relative) {
    return repositoryRoot() / relative;
}

int findClipIndexContaining(const render::GltfModelData& model, std::string_view token) {
    for (std::size_t clipIndex = 0; clipIndex < model.animations.size(); ++clipIndex) {
        if (model.animations[clipIndex].name.find(token) != std::string::npos) {
            return static_cast<int>(clipIndex);
        }
    }
    return -1;
}

void testEntityPoolReuse() {
    core::EntityPool pool;
    const core::EntityId first = pool.create();
    pool.destroy(first);
    const core::EntityId second = pool.create();

    assert(first.index == second.index);
    assert(first.generation != second.generation);
    assert(!pool.isAlive(first));
    assert(pool.isAlive(second));
}

void testComponentStoreDenseRemove() {
    core::ComponentStore<int> store;
    const core::EntityId a{0, 1};
    const core::EntityId b{1, 1};
    const core::EntityId c{2, 1};
    store.emplace(a, 10);
    store.emplace(b, 20);
    store.emplace(c, 30);
    store.remove(b);

    assert(store.size() == 2u);
    assert(store.contains(a));
    assert(!store.contains(b));
    assert(store.contains(c));
    assert(store.get(c) == 30);
}

void testEventBusOrderingAndUnsubscribe() {
    core::EventBus<core::AppEvent> bus;
    int counter = 0;
    const std::size_t subscription = bus.subscribe<core::DebugViewSelectedEvent>(
        [&counter](const core::DebugViewSelectedEvent&) { ++counter; }
    );

    bus.publish(core::DebugViewSelectedEvent{render::DebugView::Albedo});
    bus.publish(core::DebugViewSelectedEvent{render::DebugView::Normal});
    bus.dispatch();
    assert(counter == 2);

    bus.unsubscribe(subscription);
    bus.publish(core::DebugViewSelectedEvent{render::DebugView::Depth});
    bus.dispatch();
    assert(counter == 2);
}

void testCommandHistoryUndoRedoAndMerge() {
    int value = 0;
    core::CommandHistory history;
    auto applyValue = [&value](const int& snapshot) { value = snapshot; };

    history.execute(std::make_unique<core::SnapshotCommand<int>>("Set", "value", 0, 1, applyValue, true));
    history.execute(std::make_unique<core::SnapshotCommand<int>>("Set", "value", 1, 3, applyValue, true));
    assert(value == 3);
    assert(history.canUndo());

    history.undo();
    assert(value == 0);
    history.redo();
    assert(value == 3);
}

void testEditorSessionWindowVisibilityHelpers() {
    core::EditorSession session;

    assert(session.sceneHierarchyVisible);
    assert(session.inspectorWindowVisible);
    assert(session.profilerWindowVisible);

    session.showAllWindows();
    assert(session.mainWindowVisible);
    assert(session.mainWindowFocusRequested);
    assert(session.sceneHierarchyVisible);
    assert(session.inspectorWindowVisible);
    assert(session.profilerWindowVisible);
    assert(!session.sceneHierarchyFocusRequested);
    assert(!session.inspectorWindowFocusRequested);
    assert(!session.profilerWindowFocusRequested);

    session.setToolWindowsVisible(false);
    assert(!session.anyToolWindowVisible());
    assert(session.mainWindowVisible);

    session.ensureToolWindowsVisible();
    assert(session.sceneHierarchyVisible);
    assert(session.inspectorWindowVisible);
    assert(session.profilerWindowVisible);

    session.sceneHierarchyVisible = false;
    session.inspectorWindowVisible = true;
    session.profilerWindowVisible = false;
    session.suspendEditorUi();
    assert(!session.mainWindowVisible);
    assert(!session.sceneHierarchyVisible);
    assert(session.inspectorWindowVisible);
    assert(!session.profilerWindowVisible);
    assert(!session.mainWindowFocusRequested);
    assert(!session.sceneHierarchyFocusRequested);
    assert(!session.inspectorWindowFocusRequested);
    assert(!session.profilerWindowFocusRequested);
    assert(!session.textureBrowserFocusRequested);

    session.openMainWindow();
    assert(session.mainWindowVisible);
    assert(!session.sceneHierarchyVisible);
    assert(session.inspectorWindowVisible);
    assert(!session.profilerWindowVisible);
}

void testEditorSessionImGuiSettingsRoundTrip() {
    core::EditorSession session;
    session.sceneHierarchyVisible = false;
    session.inspectorWindowVisible = false;
    session.profilerWindowVisible = true;
    session.profilerFollowLatest = false;

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    core::registerEditorSessionImGuiSettings(session);
    core::markEditorSessionImGuiSettingsDirty();

    std::size_t iniSize = 0u;
    const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);
    assert(iniData != nullptr);

    const std::string ini(iniData, iniSize);
    assert(ini.find("[AlKanzar][EditorSession]") != std::string::npos);
    assert(ini.find("SceneHierarchyVisible=0") != std::string::npos);
    assert(ini.find("InspectorWindowVisible=0") != std::string::npos);
    assert(ini.find("ProfilerWindowVisible=1") != std::string::npos);
    assert(ini.find("ProfilerFollowLatest=0") != std::string::npos);
    ImGui::DestroyContext();

    core::EditorSession restored;
    restored.sceneHierarchyVisible = true;
    restored.inspectorWindowVisible = true;
    restored.profilerWindowVisible = false;
    restored.profilerFollowLatest = true;

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    core::registerEditorSessionImGuiSettings(restored);
    ImGui::LoadIniSettingsFromMemory(ini.c_str(), ini.size());
    assert(!restored.sceneHierarchyVisible);
    assert(!restored.inspectorWindowVisible);
    assert(restored.profilerWindowVisible);
    assert(!restored.profilerFollowLatest);
    ImGui::DestroyContext();
}

void testTaskSchedulerParallelForCoversFullRange() {
    core::TaskScheduler scheduler = makeScheduler();
    std::vector<int> visits(257, 0);

    core::TaskGroup group;
    scheduler.parallelFor(group, visits.size(), 13u, "Coverage", [&](std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            ++visits[index];
        }
    });
    scheduler.wait(group);

    for (int count : visits) {
        assert(count == 1);
    }
}

void testTaskSchedulerWaitCompletesScheduledGroup() {
    core::TaskScheduler scheduler = makeScheduler();
    std::vector<int> values(64, -1);

    core::TaskGroup group;
    for (std::size_t index = 0; index < values.size(); ++index) {
        scheduler.schedule(group, "Fill", [&, index]() {
            values[index] = static_cast<int>(index * 3u);
        });
    }
    scheduler.wait(group);

    assert(group.empty());
    for (std::size_t index = 0; index < values.size(); ++index) {
        assert(values[index] == static_cast<int>(index * 3u));
    }
}

void testTaskSchedulerAsyncHandleDeliversResult() {
    core::TaskScheduler scheduler = makeScheduler();
    auto handle = scheduler.submitAsync("Async Result", []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return 99;
    });

    while (!handle.ready()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const std::optional<int> value = handle.take();
    assert(value.has_value());
    assert(*value == 99);
}

void testTaskSchedulerWaitRethrowsTaskFailures() {
    core::TaskScheduler scheduler = makeScheduler();
    core::TaskGroup group;
    scheduler.schedule(group, "Explode", []() {
        throw std::runtime_error("boom");
    });

    bool threw = false;
    try {
        scheduler.wait(group);
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string_view(error.what()).find("Explode") != std::string_view::npos);
        assert(std::string_view(error.what()).find("boom") != std::string_view::npos);
    }

    assert(threw);
}

void testTaskSchedulerRepeatedPhaseWaitsComplete() {
    core::TaskScheduler scheduler = makeScheduler(4u);
    std::atomic<int> completed{0};

    constexpr std::size_t kIterations = 2048u;
    for (std::size_t iteration = 0u; iteration < kIterations; ++iteration) {
        core::TaskGroup firstPhase;
        scheduler.schedule(firstPhase, "Phase A", [&completed]() {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
        scheduler.schedule(firstPhase, "Phase B", [&completed]() {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
        scheduler.wait(firstPhase);

        core::TaskGroup secondPhase;
        scheduler.schedule(secondPhase, "Phase C", [&completed]() {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
        scheduler.wait(secondPhase);
    }

    assert(completed.load(std::memory_order_relaxed) == static_cast<int>(kIterations * 3u));
}

void testTransformHierarchyAndBounds() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    const core::EntityId parent = world.createEntity();
    const core::EntityId childA = world.createEntity();
    const core::EntityId childB = world.createEntity();

    world.transforms.emplace(parent, core::TransformComponent{glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.parents.emplace(childA, core::ParentComponent{parent});
    world.parents.emplace(childB, core::ParentComponent{parent});
    world.transforms.emplace(childB, core::TransformComponent{glm::vec3(4.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.bounds.emplace(childA, core::BoundsComponent{render::Bounds3{glm::vec3(-1.0f), glm::vec3(1.0f)}});
    world.bounds.emplace(childB, core::BoundsComponent{render::Bounds3{glm::vec3(-1.0f), glm::vec3(1.0f)}});
    world.renderables.emplace(childA, core::RenderableComponent{render::MeshHandle{0}, {}, render::RenderLayer::Geometry});
    world.renderables.emplace(childB, core::RenderableComponent{render::MeshHandle{1}, {}, render::RenderLayer::Geometry});
    world.markTransformsDirty(parent);
    world.markTransformsDirty(childA);
    world.markTransformsDirty(childB);

    core::TransformSystem system;
    system.update(world, scheduler);

    assert(world.transformCache_[childA.index].valid);
    assert(world.transformCache_[childA.index].worldBounds.min.x == 1.0f);
    assert(world.transformCache_[childA.index].worldBounds.max.x == 3.0f);
    assert(world.transformCache_[childA.index].hasWorldBounds);
    assert(world.transformCache_[childB.index].worldBounds.min.x == 5.0f);
    assert(world.transformCache_[childB.index].worldBounds.max.x == 7.0f);
    assert(world.transformCache_[parent.index].hasWorldBounds);
    assert(world.transformCache_[parent.index].worldBounds.min.x == 1.0f);
    assert(world.transformCache_[parent.index].worldBounds.max.x == 7.0f);
}

void testTransformSystemProcessesMultipleRoots() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();

    const core::EntityId rootA = world.createEntity();
    const core::EntityId childA = world.createEntity();
    const core::EntityId rootB = world.createEntity();
    const core::EntityId childB = world.createEntity();

    world.transforms.emplace(rootA, core::TransformComponent{glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(rootB, core::TransformComponent{glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.parents.emplace(childA, core::ParentComponent{rootA});
    world.parents.emplace(childB, core::ParentComponent{rootB});
    world.transforms.emplace(childA, core::TransformComponent{glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(childB, core::TransformComponent{glm::vec3(0.0f, 4.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.bounds.emplace(childA, core::BoundsComponent{render::Bounds3{glm::vec3(-1.0f), glm::vec3(1.0f)}});
    world.bounds.emplace(childB, core::BoundsComponent{render::Bounds3{glm::vec3(-1.0f), glm::vec3(1.0f)}});
    world.renderables.emplace(childA, core::RenderableComponent{render::MeshHandle{0}, {}, render::RenderLayer::Geometry});
    world.renderables.emplace(childB, core::RenderableComponent{render::MeshHandle{1}, {}, render::RenderLayer::Geometry});
    world.markTransformsDirty(rootA);
    world.markTransformsDirty(childA);
    world.markTransformsDirty(rootB);
    world.markTransformsDirty(childB);

    core::TransformSystem system;
    system.update(world, scheduler);

    assert(world.transformCache_[childA.index].worldBounds.min.x == 3.0f);
    assert(world.transformCache_[childA.index].worldBounds.max.x == 5.0f);
    assert(world.transformCache_[childB.index].worldBounds.min.y == 5.0f);
    assert(world.transformCache_[childB.index].worldBounds.max.y == 7.0f);
}

void testLightVolumeAssignment() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    world.lightVolumes.emplace_back(glm::vec3(-10.0f), glm::vec3(10.0f));
    const core::EntityId point = world.createEntity();
    const core::EntityId spot = world.createEntity();

    world.transforms.emplace(point, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(spot, core::TransformComponent{glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.pointLights.emplace(point, core::PointLightComponent{4.0f, glm::vec3(1.0f), 1.0f, 0.0f, false, false, 0.0f, 0.0f});
    world.spotLights.emplace(spot, core::SpotLightComponent{5.0f, glm::vec3(1.0f), 1.0f, glm::vec3(0.0f), 15.0f, 25.0f, 0.0f, true, false, 0.0f, 0.0f});

    core::LightSystem system;
    system.update(world, core::TimeContext{1.0f, 0.016f}, scheduler);

    assert(world.lightVolumes[0].staticLightEntities().size() == 1u);
    assert(world.lightVolumes[0].movableLightEntities().size() == 1u);
}

void testLightVolumeAssignmentIsStableAcrossLightTypes() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    world.lightVolumes.emplace_back(glm::vec3(-10.0f), glm::vec3(10.0f));

    const core::EntityId spot = world.createEntity();
    const core::EntityId point = world.createEntity();
    world.transforms.emplace(spot, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(point, core::TransformComponent{glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.spotLights.emplace(spot, core::SpotLightComponent{5.0f, glm::vec3(1.0f), 1.0f, glm::vec3(0.0f), 15.0f, 25.0f, 0.0f, false, false, 0.0f, 0.0f});
    world.pointLights.emplace(point, core::PointLightComponent{4.0f, glm::vec3(1.0f), 1.0f, 0.0f, false, false, 0.0f, 0.0f});

    core::LightSystem system;
    system.update(world, core::TimeContext{1.0f, 0.016f}, scheduler);

    const std::vector<core::EntityId>& staticLights = world.lightVolumes[0].staticLightEntities();
    assert(staticLights.size() == 2u);
    assert(staticLights[0] == spot);
    assert(staticLights[1] == point);
}

void testGltfLoaderReadsAnimatedCharacterData() {
    render::GltfModelData model{};
    assert(render::loadGltfModel(assetPath("src/render/models/Adventurer.glb").string(), model));
    assert(model.sections.size() == 5u);
    assert(model.skins.size() == 5u);
    assert(model.animations.size() == 24u);
    assert(model.nodes.size() == 86u);
    assert(model.skins[0].jointNodeIndices.size() == 62u);
    assert(model.skins[0].skeletonRootNode == 2);
    assert(model.skins[0].skeletonNodeIndices.size() > model.skins[0].jointNodeIndices.size());
    assert(model.skins[0].skeletonNodeIndices.front() == model.skins[0].skeletonRootNode);
    bool foundLowerLegEnd = false;
    bool foundFootLeft = false;
    bool foundPtLeft = false;
    for (std::size_t index = 0; index < model.skins[0].skeletonNodeIndices.size(); ++index) {
        const int nodeIndex = model.skins[0].skeletonNodeIndices[index];
        const std::string& nodeName = model.nodes[static_cast<std::size_t>(nodeIndex)].name;
        if (nodeName == "LowerLeg.L_end") {
            foundLowerLegEnd = true;
        } else if (nodeName == "Foot.L") {
            foundFootLeft = true;
            assert(model.skins[0].skeletonParentIndices[index] == 0);
        } else if (nodeName == "PT.L") {
            foundPtLeft = true;
            assert(model.skins[0].skeletonParentIndices[index] == 0);
        }
    }
    assert(foundLowerLegEnd);
    assert(foundFootLeft);
    assert(foundPtLeft);
}

void testGltfLoaderReadsStaticHouseData() {
    render::GltfModelData model{};
    assert(render::loadGltfModel(assetPath("src/render/models/FantasyHouse.glb").string(), model));
    assert(!model.sections.empty());
    assert(model.skins.empty());
    assert(model.animations.empty());
}

void testAnimationSystemDefaultsToIdleAndCrossfades() {
    auto model = std::make_shared<render::GltfModelData>();
    assert(render::loadGltfModel(assetPath("src/render/models/Adventurer.glb").string(), *model));

    core::World world;
    const core::EntityId entity = world.createEntity();
    world.animatedModels.emplace(entity, core::AnimatedModelComponent{model});

    core::AnimationSystem system;
    core::TaskScheduler scheduler = makeScheduler();
    system.update(world, core::TimeContext{0.0f, 0.0f}, scheduler, false);

    core::AnimatedModelComponent& animation = world.animatedModels.get(entity);
    assert(animation.currentClip >= 0);
    assert(model->animations[static_cast<std::size_t>(animation.currentClip)].name.find("Idle") != std::string::npos);
    assert(animation.localPose.size() == model->nodes.size());
    assert(animation.skinJointMatrices.size() == model->skins.size());

    const int runClip = findClipIndexContaining(*model, "Run");
    assert(runClip >= 0);
    animation.requestedClip = runClip;
    animation.blendDuration = 0.2f;

    system.update(world, core::TimeContext{0.0f, 0.0f}, scheduler, false);
    assert(animation.nextClip == runClip);
    assert(animation.blendElapsed == 0.0f);

    system.update(world, core::TimeContext{0.0f, 0.1f}, scheduler, false);
    assert(animation.nextClip == runClip);
    assert(animation.blendElapsed > 0.09f);
    assert(animation.currentTime > 0.0f);
    assert(animation.nextTime > 0.0f);

    system.update(world, core::TimeContext{0.0f, 0.2f}, scheduler, false);
    assert(animation.nextClip == -1);
    assert(animation.currentClip == runClip);

    animation.playing = false;
    const float pausedTime = animation.currentTime;
    system.update(world, core::TimeContext{0.0f, 0.5f}, scheduler, false);
    assert(nearlyEqual(animation.currentTime, pausedTime, 0.0001));
}

void testMaterialHandleSharingExtraction() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::MaterialLibrary materials;
    core::SelectionModel selection;
    core::RenderExtractionSystem extraction;

    auto material = std::make_shared<render::Material>();
    material->name = "Shared";
    const core::MaterialHandle handle = materials.add(material);

    const core::EntityId a = world.createEntity();
    const core::EntityId b = world.createEntity();
    world.transforms.emplace(a, core::TransformComponent{});
    world.transforms.emplace(b, core::TransformComponent{});
    world.bounds.emplace(a, core::BoundsComponent{render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)}});
    world.bounds.emplace(b, core::BoundsComponent{render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)}});
    world.renderables.emplace(a, core::RenderableComponent{render::MeshHandle{0}, handle, render::RenderLayer::Geometry});
    world.renderables.emplace(b, core::RenderableComponent{render::MeshHandle{1}, handle, render::RenderLayer::Geometry});
    world.markTransformsDirty(a);
    world.markTransformsDirty(b);

    core::TransformSystem transformSystem;
    transformSystem.update(world, scheduler);

    core::FrameSceneData frame;
    extraction.extract(world, materials, selection, frame, scheduler, true);

    assert(frame.renderables.size() == 2u);
    assert(frame.renderables[0].material == frame.renderables[1].material);
}

void testRenderExtractionPreservesOutputOrdering() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::MaterialLibrary materials;
    core::SelectionModel selection;
    core::RenderExtractionSystem extraction;
    core::TransformSystem transformSystem;
    core::LightSystem lightSystem;

    auto material = std::make_shared<render::Material>();
    const core::MaterialHandle handle = materials.add(material);

    const core::EntityId renderableA = world.createEntity();
    const core::EntityId point = world.createEntity();
    const core::EntityId renderableB = world.createEntity();
    const core::EntityId spot = world.createEntity();

    world.lightVolumes.emplace_back(glm::vec3(-10.0f), glm::vec3(10.0f));
    world.transforms.emplace(renderableA, core::TransformComponent{});
    world.transforms.emplace(renderableB, core::TransformComponent{glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(point, core::TransformComponent{});
    world.transforms.emplace(spot, core::TransformComponent{glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.bounds.emplace(renderableA, core::BoundsComponent{render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)}});
    world.bounds.emplace(renderableB, core::BoundsComponent{render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)}});
    world.renderables.emplace(renderableA, core::RenderableComponent{render::MeshHandle{0}, handle, render::RenderLayer::Geometry});
    world.renderables.emplace(renderableB, core::RenderableComponent{render::MeshHandle{1}, handle, render::RenderLayer::Actors});
    world.pointLights.emplace(point, core::PointLightComponent{4.0f, glm::vec3(1.0f), 1.0f, 0.0f, false, false, 0.0f, 0.0f});
    world.spotLights.emplace(spot, core::SpotLightComponent{5.0f, glm::vec3(1.0f), 1.0f, glm::vec3(0.0f), 15.0f, 25.0f, 0.0f, false, false, 0.0f, 0.0f});
    world.markTransformsDirty(renderableA);
    world.markTransformsDirty(renderableB);

    transformSystem.update(world, scheduler);
    lightSystem.update(world, core::TimeContext{1.0f, 0.016f}, scheduler);

    core::FrameSceneData frame;
    extraction.extract(world, materials, selection, frame, scheduler);

    assert(frame.renderables.size() == 2u);
    assert(frame.renderables[0].entity == renderableA);
    assert(frame.renderables[1].entity == renderableB);
    assert(frame.lights.size() == 2u);
    assert(frame.lights[0].entity == point);
    assert(frame.lights[1].entity == spot);
}

void testRenderExtractionPopulatesSkinnedJointRangesAndSelectionOwner() {
    auto model = std::make_shared<render::GltfModelData>();
    assert(render::loadGltfModel(assetPath("src/render/models/Adventurer.glb").string(), *model));
    assert(!model->sections.empty());
    assert(!model->skins.empty());

    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::MaterialLibrary materials;
    core::SelectionModel selection;
    core::AnimationSystem animationSystem;
    core::TransformSystem transformSystem;
    core::RenderExtractionSystem extraction;

    auto material = std::make_shared<render::Material>();
    const core::MaterialHandle materialHandle = materials.add(material);

    const core::EntityId root = world.createEntity();
    world.transforms.emplace(root, core::TransformComponent{
        glm::vec3(0.0f),
        glm::vec3(0.0f),
        glm::vec3(2.0f)
    });
    core::AnimatedModelComponent animated{model};
    animated.skinJointMatrices = {{glm::mat4(1.0f)}};
    world.animatedModels.emplace(root, std::move(animated));

    const render::GltfMeshSection& section = model->sections.front();
    const core::EntityId sectionEntity = world.createEntity();
    world.parents.emplace(sectionEntity, core::ParentComponent{root});
    world.transforms.emplace(sectionEntity, core::TransformComponent{});
    world.bounds.emplace(sectionEntity, core::BoundsComponent{render::Bounds3{glm::vec3(-0.5f), glm::vec3(0.5f)}});
    world.visibilities.emplace(sectionEntity, core::VisibilityComponent{true});
    world.renderables.emplace(sectionEntity, core::RenderableComponent{render::MeshHandle{0}, materialHandle, render::RenderLayer::Actors});
    world.skinnedRenderables.emplace(sectionEntity, core::SkinnedRenderableComponent{
        root,
        section.skinIndex,
        section.nodeIndex,
        0
    });
    world.markTransformsDirty(root);
    world.markTransformsDirty(sectionEntity);

    animationSystem.update(world, core::TimeContext{0.0f, 0.0f}, scheduler, false);
    transformSystem.update(world, scheduler, false);

    core::FrameSceneData frame;
    selection.set(sectionEntity);
    extraction.extract(world, materials, selection, frame, scheduler, false);
    assert(frame.renderables.size() == 1u);
    assert(frame.renderables[0].skinned);
    assert(world.editableTransformEntity(sectionEntity) == root);
    assert(nearlyEqual(frame.renderables[0].modelMatrix[0][0], 2.0, 0.0001));
    assert(nearlyEqual(frame.renderables[0].modelMatrix[1][1], 2.0, 0.0001));
    assert(nearlyEqual(frame.renderables[0].modelMatrix[2][2], 2.0, 0.0001));
    assert(nearlyEqual(frame.selection.transformMatrix[0][0], 2.0, 0.0001));
    assert(nearlyEqual(frame.selection.transformMatrix[1][1], 2.0, 0.0001));
    assert(nearlyEqual(frame.selection.transformMatrix[2][2], 2.0, 0.0001));
    assert(frame.renderables[0].jointMatrixBase == 0);
    assert(frame.renderables[0].jointMatrixCount == static_cast<int>(model->skins[section.skinIndex].jointNodeIndices.size()));
    assert(frame.jointMatrices.size() == model->skins[section.skinIndex].jointNodeIndices.size());
    assert(frame.selectionSkeleton.owner.has_value());
    assert(*frame.selectionSkeleton.owner == root);

    frame.clear();
    selection.set(root);
    extraction.extract(world, materials, selection, frame, scheduler, false);
    assert(frame.selectionSkeleton.owner.has_value());
    assert(*frame.selectionSkeleton.owner == root);
}

void testRenderExtractionAlignsSkinnedChildBoundsWithRenderedMesh() {
    auto model = std::make_shared<render::GltfModelData>();
    model->skins.emplace_back();
    model->sections.push_back(render::GltfMeshSection{
        "Synthetic Section",
        0,
        0,
        render::Mesh{
            std::vector<glm::vec3>{glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)},
            {},
            {},
            {},
            {},
            std::vector<glm::uvec4>{glm::uvec4(0u), glm::uvec4(0u)},
            std::vector<glm::vec4>{glm::vec4(1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)},
            {}
        },
        {}
    });

    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::MaterialLibrary materials;
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::RenderExtractionSystem extraction;

    auto material = std::make_shared<render::Material>();
    const core::MaterialHandle materialHandle = materials.add(material);

    const core::EntityId root = world.createEntity();
    world.transforms.emplace(root, core::TransformComponent{
        glm::vec3(0.0f),
        glm::vec3(0.0f),
        glm::vec3(2.0f)
    });
    core::AnimatedModelComponent animatedBounds{model};
    animatedBounds.skinJointMatrices = {{glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.0f, 0.0f))}};
    world.animatedModels.emplace(root, std::move(animatedBounds));

    const core::EntityId sectionEntity = world.createEntity();
    world.parents.emplace(sectionEntity, core::ParentComponent{root});
    world.transforms.emplace(sectionEntity, core::TransformComponent{
        glm::vec3(3.0f, 0.0f, 0.0f),
        glm::vec3(0.0f),
        glm::vec3(1.0f)
    });
    world.bounds.emplace(sectionEntity, core::BoundsComponent{
        render::Bounds3{glm::vec3(-0.5f), glm::vec3(0.5f)}
    });
    world.visibilities.emplace(sectionEntity, core::VisibilityComponent{true});
    world.renderables.emplace(sectionEntity, core::RenderableComponent{
        render::MeshHandle{0},
        materialHandle,
        render::RenderLayer::Actors
    });
    world.skinnedRenderables.emplace(sectionEntity, core::SkinnedRenderableComponent{root, 0, 0, 0});
    world.markTransformsDirty(root);
    world.markTransformsDirty(sectionEntity);

    transformSystem.update(world, scheduler, false);
    assert(nearlyEqual(world.transformCache_[sectionEntity.index].worldBounds.min.x, 5.0, 0.0001));
    assert(nearlyEqual(world.transformCache_[sectionEntity.index].worldBounds.max.x, 7.0, 0.0001));
    assert(nearlyEqual(world.transformCache_[root.index].worldBounds.min.x, 5.0, 0.0001));
    assert(nearlyEqual(world.transformCache_[root.index].worldBounds.max.x, 7.0, 0.0001));

    core::FrameSceneData frame;
    selection.set(sectionEntity);
    extraction.extract(world, materials, selection, frame, scheduler, false);

    assert(frame.renderables.size() == 1u);
    assert(frame.renderables[0].skinned);
    assert(nearlyEqual(frame.renderables[0].worldBounds.min.x, 4.0, 0.0001));
    assert(nearlyEqual(frame.renderables[0].worldBounds.max.x, 8.0, 0.0001));
    assert(nearlyEqual(frame.selection.worldBounds.min.x, 4.0, 0.0001));
    assert(nearlyEqual(frame.selection.worldBounds.max.x, 8.0, 0.0001));
    assert(nearlyEqual(frame.selection.transformMatrix[0][0], 2.0, 0.0001));

    frame.clear();
    selection.set(root);
    extraction.extract(world, materials, selection, frame, scheduler, false);
    assert(frame.selection.hasWorldBounds);
    assert(nearlyEqual(frame.selection.worldBounds.min.x, 4.0, 0.0001));
    assert(nearlyEqual(frame.selection.worldBounds.max.x, 8.0, 0.0001));
}

void testRenderExtractionFiltersHelperSkeletonBranches() {
    auto model = std::make_shared<render::GltfModelData>();
    model->nodes = {
        render::GltfNode{"Root"},
        render::GltfNode{"LowerLeg.L"},
        render::GltfNode{"LowerLeg.L_end"},
        render::GltfNode{"PT.L"},
        render::GltfNode{"PT.L_end"},
    };
    model->skins.push_back(render::SkinData{
        "SyntheticSkin",
        0,
        {0, 1},
        {-1, 0},
        {"Root", "LowerLeg.L"},
        {glm::mat4(1.0f), glm::mat4(1.0f)},
        {0, 1, 2, 3, 4},
        {-1, 0, 1, 0, 3},
        {0, 1, -1, -1, -1},
    });

    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::MaterialLibrary materials;
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::RenderExtractionSystem extraction;

    const core::EntityId root = world.createEntity();
    world.transforms.emplace(root, core::TransformComponent{});
    core::AnimatedModelComponent animated{model};
    animated.globalNodeMatrices = {
        glm::mat4(1.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.7f, 0.0f)),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.1f, 0.0f)),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.2f, -0.2f, 0.8f)),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.2f, -0.2f, 1.2f)),
    };
    animated.skinJointMatrices = {{glm::mat4(1.0f), glm::mat4(1.0f)}};
    world.animatedModels.emplace(root, std::move(animated));
    world.markTransformsDirty(root);

    transformSystem.update(world, scheduler, false);

    core::FrameSceneData frame;
    selection.set(root);
    extraction.extract(world, materials, selection, frame, scheduler, false);

    assert(frame.selectionSkeleton.owner.has_value());
    assert(*frame.selectionSkeleton.owner == root);
    assert(frame.selectionSkeleton.jointNames.size() == 3u);
    assert(frame.selectionSkeleton.jointNames[0] == "Root");
    assert(frame.selectionSkeleton.jointNames[1] == "LowerLeg.L");
    assert(frame.selectionSkeleton.jointNames[2] == "LowerLeg.L_end");
    assert(frame.selectionSkeleton.parentIndices.size() == 3u);
    assert(frame.selectionSkeleton.parentIndices[0] == -1);
    assert(frame.selectionSkeleton.parentIndices[1] == 0);
    assert(frame.selectionSkeleton.parentIndices[2] == 1);
    assert(std::find(
        frame.selectionSkeleton.jointNames.begin(),
        frame.selectionSkeleton.jointNames.end(),
        std::string("PT.L")
    ) == frame.selectionSkeleton.jointNames.end());
    assert(std::find(
        frame.selectionSkeleton.jointNames.begin(),
        frame.selectionSkeleton.jointNames.end(),
        std::string("PT.L_end")
    ) == frame.selectionSkeleton.jointNames.end());
}

void testRenderSceneViewBuildResolvesRenderableSelection() {
    core::TaskScheduler scheduler = makeScheduler();
    core::FrameSceneData frame;
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{1, 1},
        render::MeshHandle{0},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)},
        render::Bounds3{glm::vec3(2.0f), glm::vec3(3.0f)},
        glm::mat4(1.0f),
        true
    });
    frame.selection.entity = core::EntityId{1, 1};
    frame.selection.worldBounds = render::Bounds3{glm::vec3(2.0f), glm::vec3(3.0f)};
    frame.selection.transformMatrix = glm::mat4(2.0f);

    const render::RenderSceneView scene = render::buildRenderSceneView(
        frame,
        std::vector<const render::MeshBuffer*>{nullptr},
        scheduler
    );
    assert(scene.objects.size() == 1u);
    assert(scene.selection.kind == render::RenderSelectionKind::Renderable);
    assert(scene.selection.index == 0);
    assert(scene.selection.transformMatrix[0][0] == 2.0f);
}

void testRenderSceneViewBuildResolvesLightSelection() {
    core::TaskScheduler scheduler = makeScheduler();
    core::FrameSceneData frame;
    frame.lights.push_back(core::FrameLight{core::EntityId{4, 1}, render::LightType::Point});
    frame.selection.entity = core::EntityId{4, 1};

    const render::RenderSceneView scene = render::buildRenderSceneView(frame, {}, scheduler);
    assert(scene.selection.kind == render::RenderSelectionKind::Light);
    assert(scene.selection.index == 0);
}

void testRenderSceneViewBuildResolvesNodeSelection() {
    core::TaskScheduler scheduler = makeScheduler();
    core::FrameSceneData frame;
    frame.selection.entity = core::EntityId{9, 1};
    frame.selection.worldBounds = render::Bounds3{glm::vec3(1.0f), glm::vec3(4.0f)};
    frame.selection.hasWorldBounds = true;
    frame.selection.transformMatrix = glm::mat4(3.0f);

    const render::RenderSceneView scene = render::buildRenderSceneView(frame, {}, scheduler);
    assert(scene.selection.kind == render::RenderSelectionKind::Node);
    assert(scene.selection.index == -1);
    assert(scene.selection.hasWorldBounds);
    assert(scene.selection.worldBounds.min.x == 1.0f);
    assert(scene.selection.worldBounds.max.x == 4.0f);
    assert(scene.selection.transformMatrix[0][0] == 3.0f);
}

void runFramePreparationIterations(std::size_t workerCount) {
    core::TaskScheduler scheduler = makeScheduler(workerCount);
    core::World world;
    core::MaterialLibrary materials;
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::LightSystem lightSystem;
    core::RenderExtractionSystem extraction;
    core::FrameSceneData frame;

    auto material = std::make_shared<render::Material>();
    material->name = "Frame Prep";
    const core::MaterialHandle materialHandle = materials.add(material);

    const core::EntityId root = world.createEntity();
    const core::EntityId renderableA = world.createEntity();
    const core::EntityId renderableB = world.createEntity();
    const core::EntityId point = world.createEntity();
    const core::EntityId spot = world.createEntity();

    world.transforms.emplace(root, core::TransformComponent{glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.parents.emplace(renderableA, core::ParentComponent{root});
    world.transforms.emplace(renderableA, core::TransformComponent{glm::vec3(0.5f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(renderableB, core::TransformComponent{glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(point, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(spot, core::TransformComponent{glm::vec3(1.0f, 2.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.bounds.emplace(renderableA, core::BoundsComponent{render::Bounds3{glm::vec3(-0.5f), glm::vec3(0.5f)}});
    world.bounds.emplace(renderableB, core::BoundsComponent{render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)}});
    world.renderables.emplace(renderableA, core::RenderableComponent{render::MeshHandle{0}, materialHandle, render::RenderLayer::Geometry});
    world.renderables.emplace(renderableB, core::RenderableComponent{render::MeshHandle{1}, materialHandle, render::RenderLayer::Actors});
    world.pointLights.emplace(point, core::PointLightComponent{4.0f, glm::vec3(1.0f), 1.0f, 0.0f, false, false, 0.0f, 0.0f});
    world.spotLights.emplace(
        spot,
        core::SpotLightComponent{
            6.0f,
            glm::vec3(1.0f),
            1.0f,
            glm::vec3(0.0f, 0.0f, 0.0f),
            15.0f,
            25.0f,
            0.0f,
            true,
            false,
            0.0f,
            0.0f
        }
    );
    world.lightVolumes.emplace_back(glm::vec3(-10.0f), glm::vec3(10.0f));

    const std::vector<const render::MeshBuffer*> meshLookup{nullptr, nullptr};
    glm::vec3 previousRootPosition = world.transforms.get(root).position;

    for (int iteration = 0; iteration < 4; ++iteration) {
        world.transforms.get(root).position.x += 0.25f;
        world.transforms.get(renderableB).position.y += 0.5f;
        world.markTransformsDirty(root);
        world.markTransformsDirty(renderableB);
        world.markLightsDirty(point);
        world.markLightsDirty(spot);

        const core::TimeContext timeContext{static_cast<float>(iteration) * 0.25f, 1.0f / 60.0f};
        transformSystem.update(world, scheduler);
        lightSystem.update(world, timeContext, scheduler);
        extraction.extract(world, materials, selection, frame, scheduler);
        const render::RenderSceneView scene = render::buildRenderSceneView(frame, meshLookup, scheduler);

        assert(frame.renderables.size() == 2u);
        assert(frame.renderables[0].entity == renderableA);
        assert(frame.renderables[1].entity == renderableB);
        assert(frame.lights.size() == 2u);
        assert(frame.lights[0].entity == point);
        assert(frame.lights[1].entity == spot);
        assert(frame.lightVolumes.size() == 1u);
        assert(scene.objects.size() == 2u);
        assert(scene.objects[0].sourceIndex == 0);
        assert(scene.objects[1].sourceIndex == 1);
        assert(scene.lights.size() == 2u);
        assert(scene.lightVolumes.size() == 1u);
        assert(world.transformCache_[renderableA.index].valid);
        assert(world.transformCache_[renderableB.index].valid);
        assert(world.lightVolumes[0].staticLightEntities().size() == 1u);
        assert(world.lightVolumes[0].movableLightEntities().size() == 1u);
        assert(world.transforms.get(root).position.x > previousRootPosition.x);
        previousRootPosition = world.transforms.get(root).position;
    }
}

void testFramePreparationRemainsStableWithSingleWorker() {
    runFramePreparationIterations(1u);
}

void testFramePreparationRemainsStableWithMultipleWorkers() {
    runFramePreparationIterations(4u);
}

void testActiveLightSelectionDeduplicatesAcrossVolumes() {
    std::vector<core::FrameLight> lights{
        core::FrameLight{core::EntityId{1, 1}, render::LightType::Point},
        core::FrameLight{core::EntityId{2, 1}, render::LightType::Spot},
        core::FrameLight{core::EntityId{3, 1}, render::LightType::Point},
    };
    std::vector<core::FrameLightVolume> volumes{
        core::FrameLightVolume{{}, {}, {0, 1}, {2}},
        core::FrameLightVolume{{}, {}, {1}, {2}},
    };

    const render::ActiveLightSelection selection = render::selectActiveLights(lights, volumes);
    assert(selection.indices.size() == 3u);
    assert(selection.pointLightCount == 2);
    assert(selection.spotLightCount == 1);
    assert(selection.indices[0] == 0);
    assert(selection.indices[1] == 1);
    assert(selection.indices[2] == 2);
}

void testPickingSystemCanIgnoreLights() {
    core::FrameSceneData frame;
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{1, 1},
        render::MeshHandle{0},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(0.0f)},
        render::Bounds3{glm::vec3(-0.25f, -0.25f, -6.0f), glm::vec3(0.25f, 0.25f, -4.0f)},
        glm::mat4(1.0f),
        true
    });
    frame.lights.push_back(core::FrameLight{
        core::EntityId{2, 1},
        render::LightType::Point,
        glm::vec3(0.0f, 0.0f, -2.0f),
        0.75f,
        glm::vec3(1.0f),
        1.0f
    });

    render::CameraMatrices camera{};
    camera.projection = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 10.0f);
    camera.invProjection = glm::inverse(camera.projection);
    camera.view = glm::mat4(1.0f);

    core::PickingSystem picking;
    const std::optional<core::EntityId> withLights = picking.pick(frame, camera, 100, 100, 50, 50, true);
    const std::optional<core::EntityId> withoutLights = picking.pick(frame, camera, 100, 100, 50, 50, false);

    assert(withLights.has_value());
    assert(withLights->index == 2u);
    assert(withoutLights.has_value());
    assert(withoutLights->index == 1u);
}

void testProfilerScopeTreeAggregatesNestedScopes() {
    const std::uint64_t mainThreadId = 77u;
    const std::vector<core::ProfilerRecordedScope> scopes{
        core::ProfilerRecordedScope{"Frame", core::ProfilerRecordedScope::kRoot, 0u, 100u, mainThreadId},
        core::ProfilerRecordedScope{"Update", 0u, 10u, 70u, mainThreadId},
        core::ProfilerRecordedScope{"Transform", 1u, 20u, 30u, mainThreadId},
        core::ProfilerRecordedScope{"Transform", 1u, 40u, 50u, mainThreadId},
        core::ProfilerRecordedScope{"Render", 0u, 75u, 95u, mainThreadId},
    };

    const std::vector<core::ProfilerScopeNode> tree = core::buildProfilerScopeTree(scopes, mainThreadId);
    assert(tree.size() == 1u);
    assert(tree[0].name == "Frame");
    assert(tree[0].callCount == 1u);
    assert(nearlyEqual(tree[0].totalMs, 0.0001));
    assert(nearlyEqual(tree[0].selfMs, 0.00002));
    assert(tree[0].children.size() == 2u);

    const core::ProfilerScopeNode& update = tree[0].children[0];
    assert(update.name == "Update");
    assert(update.callCount == 1u);
    assert(nearlyEqual(update.totalMs, 0.00006));
    assert(nearlyEqual(update.selfMs, 0.00004));
    assert(update.children.size() == 1u);

    const core::ProfilerScopeNode& transform = update.children[0];
    assert(transform.name == "Transform");
    assert(transform.callCount == 2u);
    assert(nearlyEqual(transform.totalMs, 0.00002));
    assert(nearlyEqual(transform.selfMs, 0.00002));
    assert(transform.thread == "Main");
}

void testProfilerServiceStartStopAndFrameRingBuffer() {
    core::ProfilerConfig config{};
    config.maxFrames = 2u;
    config.maxCpuScopesPerFrame = 8u;
    core::ProfilerService profiler(config);

    profiler.startCapture();
    for (int frame = 0; frame < 3; ++frame) {
        profiler.beginFrame();
        {
            auto scope = profiler.scopedCpu("Frame");
            (void)scope;
        }
        profiler.endFrame({});
    }
    profiler.waitForWorkerIdle();

    auto snapshots = profiler.snapshots();
    assert(snapshots.size() == 2u);
    assert(snapshots.front()->frameNumber == 2u);
    assert(snapshots.back()->frameNumber == 3u);

    profiler.stopCapture();
    profiler.beginFrame();
    profiler.endFrame({});
    profiler.waitForWorkerIdle();

    snapshots = profiler.snapshots();
    assert(snapshots.size() == 2u);
    assert(snapshots.back()->frameNumber == 3u);
    assert(!profiler.stats().capturing);
}

void testProfilerServiceDropsExcessCpuScopes() {
    core::ProfilerConfig config{};
    config.maxFrames = 4u;
    config.maxCpuScopesPerFrame = 1u;
    core::ProfilerService profiler(config);

    profiler.startCapture();
    profiler.beginFrame();
    {
        auto rootScope = profiler.scopedCpu("Root");
        auto droppedScope = profiler.scopedCpu("Dropped");
        (void)rootScope;
        (void)droppedScope;
    }
    profiler.endFrame({});
    profiler.waitForWorkerIdle();

    const auto snapshots = profiler.snapshots();
    assert(snapshots.size() == 1u);
    assert(snapshots[0]->cpuScopes.size() == 1u);
    assert(snapshots[0]->cpuScopes[0].name == "Root");
    assert(profiler.stats().droppedCpuScopes == 1u);
}

void testProfilerServiceCapturesWorkerThreadScopes() {
    core::ProfilerConfig config{};
    config.maxFrames = 4u;
    config.maxCpuScopesPerFrame = 64u;
    core::ProfilerService profiler(config);
    core::TaskScheduler scheduler = makeScheduler();
    scheduler.setProfiler(&profiler);

    profiler.startCapture();
    profiler.beginFrame();

    core::TaskGroup group;
    std::atomic<int> finished{0};
    for (int task = 0; task < 8; ++task) {
        scheduler.schedule(group, "Worker Task", [&finished]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            finished.fetch_add(1, std::memory_order_acq_rel);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    scheduler.wait(group);
    assert(finished.load(std::memory_order_acquire) == 8);

    profiler.endFrame({});
    profiler.waitForWorkerIdle();

    const auto snapshots = profiler.snapshots();
    assert(snapshots.size() == 1u);
    assert(containsNonMainThreadScope(snapshots[0]->cpuScopes));
}

void testProfilerServiceRetainsRawFramesForExport() {
    core::ProfilerConfig config{};
    config.maxFrames = 2u;
    config.maxCpuScopesPerFrame = 8u;
    core::ProfilerService profiler(config);

    profiler.startCapture();
    for (int frame = 0; frame < 3; ++frame) {
        profiler.beginFrame();
        {
            auto scope = profiler.scopedCpu(frame == 2 ? "Third" : "Frame");
            (void)scope;
        }
        profiler.endFrame({
            render::ResourceMemoryRecord{
                "Shadow Atlas",
                "Texture",
                0u,
                static_cast<std::uint64_t>(1024u * (frame + 1))
            }
        });
    }
    profiler.waitForWorkerIdle();

    const core::ProfilerTraceCapture capture = profiler.rawCapture();
    assert(capture.mainThreadId == profiler.mainThreadId());
    assert(capture.frames.size() == 2u);
    assert(capture.frames.front().frameNumber == 2u);
    assert(capture.frames.back().frameNumber == 3u);
    assert(capture.frames.back().cpuScopes.size() == 1u);
    assert(capture.frames.back().cpuScopes[0].name == "Third");
    assert(capture.frames.back().resources.size() == 1u);
    assert(capture.frames.back().resources[0].gpuBytes == 3072u);
    assert(capture.frames.back().endNs >= capture.frames.back().startNs);
}

void testPerfettoTraceExporterWritesTrackEventsAndCounters() {
    core::ProfilerTraceCapture capture{};
    capture.sessionId = 42u;
    capture.mainThreadId = 77u;
    capture.frames = {
        core::ProfilerTraceFrame{
            42u,
            1u,
            100u,
            200u,
            0.0,
            {
                core::ProfilerRecordedScope{"Frame", core::ProfilerRecordedScope::kRoot, 100u, 200u, 77u},
                core::ProfilerRecordedScope{"Update", 0u, 110u, 150u, 77u},
                core::ProfilerRecordedScope{"Worker Task", core::ProfilerRecordedScope::kRoot, 120u, 180u, 88u},
            },
            {
                core::GpuPassSample{"GBuffer", 2.5, true, false},
                core::GpuPassSample{"Lighting", 1.25, true, false},
            },
            {
                core::ResourceMemoryEntry{"Shadow Atlas", "Texture", 0u, 4096u},
            }
        },
        core::ProfilerTraceFrame{
            42u,
            2u,
            300u,
            450u,
            0.0,
            {
                core::ProfilerRecordedScope{"Frame", core::ProfilerRecordedScope::kRoot, 300u, 450u, 77u},
            },
            {
                core::GpuPassSample{"GBuffer", 3.0, true, false},
            },
            {
                core::ResourceMemoryEntry{"Shadow Atlas", "Texture", 0u, 8192u},
                core::ResourceMemoryEntry{"Mesh Cache", "CPU", 1024u, 0u},
            }
        }
    };

    const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "alkanzar-profiler-test.pftrace";
    std::error_code removeError{};
    std::filesystem::remove(outputPath, removeError);

    std::string exportError{};
    assert(core::exportProfilerTraceCaptureToPerfetto(capture, outputPath.string(), &exportError));
    assert(exportError.empty());
    assert(std::filesystem::exists(outputPath));

    const std::string bytes = readFileToString(outputPath);
    assert(!bytes.empty());
    assert(bytes.find("Frames") != std::string::npos);
    assert(bytes.find("Main") != std::string::npos);
    assert(bytes.find("T88") != std::string::npos);
    assert(bytes.find("GPU Frame") != std::string::npos);
    assert(bytes.find("Texture: Shadow Atlas (GPU)") != std::string::npos);
    assert(bytes.find("CPU: Mesh Cache (RAM)") != std::string::npos);

    const std::vector<std::string_view> packets = collectMessageFieldValues(bytes, 1u);
    assert(!packets.empty());

    std::size_t descriptorCount = 0u;
    std::size_t sliceBeginCount = 0u;
    std::size_t sliceEndCount = 0u;
    std::size_t counterCount = 0u;
    for (std::string_view packet : packets) {
        descriptorCount += collectMessageFieldValues(packet, 60u).size();
        for (std::string_view event : collectMessageFieldValues(packet, 11u)) {
            const std::optional<std::uint64_t> type = firstVarintFieldValue(event, 9u);
            assert(type.has_value());
            if (*type == 1u) {
                ++sliceBeginCount;
            } else if (*type == 2u) {
                ++sliceEndCount;
            } else if (*type == 4u) {
                ++counterCount;
            }
        }
    }

    assert(descriptorCount >= 9u);
    assert(sliceBeginCount == 6u);
    assert(sliceEndCount == 6u);
    assert(counterCount == 8u);

    std::filesystem::remove(outputPath, removeError);
}

void testPerfettoTraceExporterFailsForInvalidOutputPath() {
    core::ProfilerTraceCapture capture{};
    capture.sessionId = 3u;
    capture.mainThreadId = 1u;
    capture.frames.push_back(core::ProfilerTraceFrame{
        3u,
        1u,
        10u,
        20u,
        0.0,
        {},
        {},
        {}
    });

    const std::filesystem::path blockingPath = std::filesystem::temp_directory_path() / "alkanzar-profiler-blocking-file";
    {
        std::ofstream output(blockingPath, std::ios::binary | std::ios::trunc);
        assert(output.is_open());
        output << "blocking";
    }

    std::string exportError{};
    const bool exported = core::exportProfilerTraceCaptureToPerfetto(
        capture,
        (blockingPath / "trace.pftrace").string(),
        &exportError
    );
    assert(!exported);
    assert(!exportError.empty());

    std::error_code removeError{};
    std::filesystem::remove(blockingPath, removeError);
}

void testProfilingMemoryEstimators() {
    render::Texture texture{};
    texture.width = 4;
    texture.height = 4;
    texture.format = render::Format::RGBA8;
    texture.bytes.resize(4u * 4u * 4u);

    const render::Mesh mesh{
        std::vector<glm::vec3>(2, glm::vec3(0.0f)),
        std::vector<glm::vec3>(2, glm::vec3(0.0f, 1.0f, 0.0f)),
        std::vector<glm::vec4>(2, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)),
        std::vector<std::vector<glm::vec2>>(2, std::vector<glm::vec2>(2, glm::vec2(0.0f))),
        std::vector<glm::vec4>(2, glm::vec4(1.0f)),
        std::vector<glm::uvec4>(2, glm::uvec4(0u)),
        std::vector<glm::vec4>(2, glm::vec4(0.0f)),
        std::vector<unsigned int>{0u, 1u, 0u}
    };

    const std::uint64_t textureCpuBytes = render::estimateTextureCpuBytes(texture);
    const std::uint64_t textureGpuBytes = render::estimateTextureGpuBytes(texture);
    const std::uint64_t shadowMapBytes = render::estimateTextureStorageBytes(16, 16, 4, render::TextureStorageFormat::Depth24);
    const std::uint64_t renderTargetBytes = render::estimateTextureStorageBytes(8, 4, 1, render::TextureStorageFormat::Depth24Stencil8);
    const render::MeshBufferMemoryEstimate meshBytes = render::estimateMeshBufferBytes(mesh);

    assert(textureCpuBytes == 64u);
    assert(textureGpuBytes == 84u);
    assert(shadowMapBytes == 4096u);
    assert(renderTargetBytes == 128u);
    assert(
        meshBytes.vertexBytes ==
        static_cast<std::uint64_t>(2u * (22u * sizeof(float) + 4u * sizeof(std::uint32_t)))
    );
    assert(meshBytes.indexBytes == static_cast<std::uint64_t>(3u * sizeof(unsigned int)));
}

}  // namespace

int main() {
    testEntityPoolReuse();
    testComponentStoreDenseRemove();
    testEventBusOrderingAndUnsubscribe();
    testCommandHistoryUndoRedoAndMerge();
    testEditorSessionWindowVisibilityHelpers();
    testEditorSessionImGuiSettingsRoundTrip();
    testTaskSchedulerParallelForCoversFullRange();
    testTaskSchedulerWaitCompletesScheduledGroup();
    testTaskSchedulerAsyncHandleDeliversResult();
    testTaskSchedulerWaitRethrowsTaskFailures();
    testTaskSchedulerRepeatedPhaseWaitsComplete();
    testTransformHierarchyAndBounds();
    testTransformSystemProcessesMultipleRoots();
    testLightVolumeAssignment();
    testLightVolumeAssignmentIsStableAcrossLightTypes();
    testMaterialHandleSharingExtraction();
    testRenderExtractionPreservesOutputOrdering();
    testRenderExtractionPopulatesSkinnedJointRangesAndSelectionOwner();
    testRenderExtractionAlignsSkinnedChildBoundsWithRenderedMesh();
    testRenderExtractionFiltersHelperSkeletonBranches();
    testRenderSceneViewBuildResolvesRenderableSelection();
    testRenderSceneViewBuildResolvesLightSelection();
    testRenderSceneViewBuildResolvesNodeSelection();
    testFramePreparationRemainsStableWithSingleWorker();
    testFramePreparationRemainsStableWithMultipleWorkers();
    testActiveLightSelectionDeduplicatesAcrossVolumes();
    testPickingSystemCanIgnoreLights();
    testProfilerScopeTreeAggregatesNestedScopes();
    testProfilerServiceStartStopAndFrameRingBuffer();
    testProfilerServiceDropsExcessCpuScopes();
    testProfilerServiceCapturesWorkerThreadScopes();
    testProfilerServiceRetainsRawFramesForExport();
    testPerfettoTraceExporterWritesTrackEventsAndCounters();
    testPerfettoTraceExporterFailsForInvalidOutputPath();
    testProfilingMemoryEstimators();
    return EXIT_SUCCESS;
}
