#include <cassert>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <variant>

#include <glm/ext/matrix_clip_space.hpp>

#include "core/CommandHistory.hpp"
#include "core/ComponentStore.hpp"
#include "core/EventBus.hpp"
#include "core/Events.hpp"
#include "core/LightSystem.hpp"
#include "core/MaterialLibrary.hpp"
#include "core/PickingSystem.hpp"
#include "core/ProfilerService.hpp"
#include "core/RenderExtractionSystem.hpp"
#include "core/SelectionModel.hpp"
#include "core/TransformSystem.hpp"
#include "core/World.hpp"
#include "render/Profiling.hpp"
#include "render/RenderLightPipeline.hpp"
#include "render/RenderSceneView.hpp"

namespace {

bool nearlyEqual(double lhs, double rhs, double epsilon = 0.001) {
    return std::abs(lhs - rhs) <= epsilon;
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

void testTransformHierarchyAndBounds() {
    core::World world;
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
    system.update(world);

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

void testLightVolumeAssignment() {
    core::World world;
    world.lightVolumes.emplace_back(glm::vec3(-10.0f), glm::vec3(10.0f));
    const core::EntityId point = world.createEntity();
    const core::EntityId spot = world.createEntity();

    world.transforms.emplace(point, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(spot, core::TransformComponent{glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.pointLights.emplace(point, core::PointLightComponent{4.0f, glm::vec3(1.0f), 1.0f, 0.0f, false, false, 0.0f, 0.0f});
    world.spotLights.emplace(spot, core::SpotLightComponent{5.0f, glm::vec3(1.0f), 1.0f, glm::vec3(0.0f), 15.0f, 25.0f, 0.0f, true, false, 0.0f, 0.0f});

    core::LightSystem system;
    system.update(world, core::TimeContext{1.0f, 0.016f});

    assert(world.lightVolumes[0].staticLightEntities().size() == 1u);
    assert(world.lightVolumes[0].movableLightEntities().size() == 1u);
}

void testMaterialHandleSharingExtraction() {
    core::World world;
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
    transformSystem.update(world);

    core::FrameSceneData frame;
    extraction.extract(world, materials, selection, frame);

    assert(frame.renderables.size() == 2u);
    assert(frame.renderables[0].material == frame.renderables[1].material);
}

void testRenderSceneViewBuildResolvesRenderableSelection() {
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

    const render::RenderSceneView scene = render::buildRenderSceneView(frame, std::vector<const render::MeshBuffer*>{nullptr});
    assert(scene.objects.size() == 1u);
    assert(scene.selection.kind == render::RenderSelectionKind::Renderable);
    assert(scene.selection.index == 0);
    assert(scene.selection.transformMatrix[0][0] == 2.0f);
}

void testRenderSceneViewBuildResolvesLightSelection() {
    core::FrameSceneData frame;
    frame.lights.push_back(core::FrameLight{core::EntityId{4, 1}, render::LightType::Point});
    frame.selection.entity = core::EntityId{4, 1};

    const render::RenderSceneView scene = render::buildRenderSceneView(frame, {});
    assert(scene.selection.kind == render::RenderSelectionKind::Light);
    assert(scene.selection.index == 0);
}

void testRenderSceneViewBuildResolvesNodeSelection() {
    core::FrameSceneData frame;
    frame.selection.entity = core::EntityId{9, 1};
    frame.selection.worldBounds = render::Bounds3{glm::vec3(1.0f), glm::vec3(4.0f)};
    frame.selection.hasWorldBounds = true;
    frame.selection.transformMatrix = glm::mat4(3.0f);

    const render::RenderSceneView scene = render::buildRenderSceneView(frame, {});
    assert(scene.selection.kind == render::RenderSelectionKind::Node);
    assert(scene.selection.index == -1);
    assert(scene.selection.hasWorldBounds);
    assert(scene.selection.worldBounds.min.x == 1.0f);
    assert(scene.selection.worldBounds.max.x == 4.0f);
    assert(scene.selection.transformMatrix[0][0] == 3.0f);
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
    assert(meshBytes.vertexBytes == static_cast<std::uint64_t>(2u * 18u * sizeof(float)));
    assert(meshBytes.indexBytes == static_cast<std::uint64_t>(3u * sizeof(unsigned int)));
}

}  // namespace

int main() {
    testEntityPoolReuse();
    testComponentStoreDenseRemove();
    testEventBusOrderingAndUnsubscribe();
    testCommandHistoryUndoRedoAndMerge();
    testTransformHierarchyAndBounds();
    testLightVolumeAssignment();
    testMaterialHandleSharingExtraction();
    testRenderSceneViewBuildResolvesRenderableSelection();
    testRenderSceneViewBuildResolvesLightSelection();
    testRenderSceneViewBuildResolvesNodeSelection();
    testActiveLightSelectionDeduplicatesAcrossVolumes();
    testPickingSystemCanIgnoreLights();
    testProfilerScopeTreeAggregatesNestedScopes();
    testProfilerServiceStartStopAndFrameRingBuffer();
    testProfilerServiceDropsExcessCpuScopes();
    testProfilingMemoryEstimators();
    return EXIT_SUCCESS;
}
