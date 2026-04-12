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
#include <unordered_map>
#include <variant>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <imgui.h>

#include "core/animation/AnimationSystem.hpp"
#include "core/app/RuntimePolicy.hpp"
#include "core/editor/CommandHistory.hpp"
#include "core/editor/EditorSession.hpp"
#include "core/editor/EditorSessionImGuiSettings.hpp"
#include "core/ecs/ComponentStore.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/lighting/LightSystem.hpp"
#include "core/lighting/MaterialLibrary.hpp"
#include "core/navigation/Navigation.hpp"
#include "core/scene/Camera.hpp"
#include "core/systems/PickingSystem.hpp"
#include "core/profiling/ProfilerService.hpp"
#include "core/systems/RenderExtractionSystem.hpp"
#include "core/editor/SelectionModel.hpp"
#include "core/systems/TaskScheduler.hpp"
#include "core/transform/TransformSystem.hpp"
#include "core/ecs/World.hpp"
#include "render/resources/Profiling.hpp"
#include "render/resources/StaticGltfModel.hpp"
#include "render/pipeline/SceneOverlayRenderer.hpp"
#include "render/pipeline/RenderLightPipeline.hpp"
#include "render/engine/RenderSceneView.hpp"

namespace {

bool nearlyEqual(double lhs, double rhs, double epsilon = 0.001) {
    return std::abs(lhs - rhs) <= epsilon;
}

render::Bounds3 exactSkinnedWorldBounds(
    const render::Mesh& mesh,
    const std::vector<glm::mat4>& skinMatrices,
    const glm::mat4& modelMatrix
) {
    render::Bounds3 outBounds{};
    bool hasBounds = false;
    for (std::size_t vertexIndex = 0; vertexIndex < mesh.positions.size(); ++vertexIndex) {
        glm::vec4 skinnedPosition(mesh.positions[vertexIndex], 1.0f);
        if (vertexIndex < mesh.jointIndices.size() && vertexIndex < mesh.jointWeights.size()) {
            const glm::uvec4 joints = mesh.jointIndices[vertexIndex];
            const glm::vec4 weights = mesh.jointWeights[vertexIndex];
            const float totalWeight = weights.x + weights.y + weights.z + weights.w;
            if (totalWeight > 0.0f) {
                glm::mat4 skinMatrix(0.0f);
                if (weights.x > 0.0f && joints.x < skinMatrices.size()) {
                    skinMatrix += skinMatrices[joints.x] * weights.x;
                }
                if (weights.y > 0.0f && joints.y < skinMatrices.size()) {
                    skinMatrix += skinMatrices[joints.y] * weights.y;
                }
                if (weights.z > 0.0f && joints.z < skinMatrices.size()) {
                    skinMatrix += skinMatrices[joints.z] * weights.z;
                }
                if (weights.w > 0.0f && joints.w < skinMatrices.size()) {
                    skinMatrix += skinMatrices[joints.w] * weights.w;
                }
                skinnedPosition = skinMatrix * skinnedPosition;
            }
        }

        const glm::vec3 worldPosition = glm::vec3(modelMatrix * skinnedPosition);
        if (!hasBounds) {
            outBounds.min = worldPosition;
            outBounds.max = worldPosition;
            hasBounds = true;
            continue;
        }
        outBounds.min = glm::min(outBounds.min, worldPosition);
        outBounds.max = glm::max(outBounds.max, worldPosition);
    }

    assert(hasBounds);
    return outBounds;
}

bool boundsContain(const render::Bounds3& outer, const render::Bounds3& inner, double epsilon = 0.0001) {
    return outer.min.x <= inner.min.x + epsilon &&
        outer.min.y <= inner.min.y + epsilon &&
        outer.min.z <= inner.min.z + epsilon &&
        outer.max.x >= inner.max.x - epsilon &&
        outer.max.y >= inner.max.y - epsilon &&
        outer.max.z >= inner.max.z - epsilon;
}

core::TaskScheduler makeScheduler(std::size_t workerCount = 2u) {
    return core::TaskScheduler(core::TaskSchedulerConfig{workerCount});
}

render::CameraMatrices makeOrthoCamera(
    float left = -1.0f,
    float right = 1.0f,
    float bottom = -1.0f,
    float top = 1.0f,
    float nearPlane = 0.1f,
    float farPlane = 10.0f
) {
    render::CameraMatrices camera{};
    camera.projection = glm::ortho(left, right, bottom, top, nearPlane, farPlane);
    camera.invProjection = glm::inverse(camera.projection);
    camera.view = glm::mat4(1.0f);
    return camera;
}

render::Mesh makeHorizontalQuadMesh(float minX, float maxX, float minZ, float maxZ, float y) {
    render::Mesh mesh{};
    mesh.uvSets.resize(2);
    mesh.positions = {
        glm::vec3(minX, y, minZ),
        glm::vec3(minX, y, maxZ),
        glm::vec3(maxX, y, maxZ),
        glm::vec3(maxX, y, minZ),
    };
    mesh.normals = std::vector<glm::vec3>(4u, glm::vec3(0.0f, 1.0f, 0.0f));
    mesh.colors = std::vector<glm::vec4>(4u, glm::vec4(1.0f));
    mesh.uvSets[0] = {
        glm::vec2(0.0f, 0.0f),
        glm::vec2(0.0f, 1.0f),
        glm::vec2(1.0f, 1.0f),
        glm::vec2(1.0f, 0.0f),
    };
    mesh.uvSets[1] = std::vector<glm::vec2>(4u, glm::vec2(0.0f));
    mesh.indices = {0u, 1u, 2u, 0u, 2u, 3u};
    return mesh;
}

render::Mesh makeBoxVertexMesh(const glm::vec3& minPoint, const glm::vec3& maxPoint) {
    render::Mesh mesh{};
    mesh.positions = {
        glm::vec3(minPoint.x, minPoint.y, minPoint.z),
        glm::vec3(maxPoint.x, minPoint.y, minPoint.z),
        glm::vec3(minPoint.x, maxPoint.y, minPoint.z),
        glm::vec3(maxPoint.x, maxPoint.y, minPoint.z),
        glm::vec3(minPoint.x, minPoint.y, maxPoint.z),
        glm::vec3(maxPoint.x, minPoint.y, maxPoint.z),
        glm::vec3(minPoint.x, maxPoint.y, maxPoint.z),
        glm::vec3(maxPoint.x, maxPoint.y, maxPoint.z),
    };
    return mesh;
}

render::Mesh makeSlopedTriangleMesh() {
    render::Mesh mesh{};
    mesh.uvSets.resize(2);
    mesh.positions = {
        glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(1.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
    };
    mesh.normals = std::vector<glm::vec3>(3u, glm::normalize(glm::cross(mesh.positions[1] - mesh.positions[0], mesh.positions[2] - mesh.positions[0])));
    mesh.colors = std::vector<glm::vec4>(3u, glm::vec4(1.0f));
    mesh.uvSets[0] = std::vector<glm::vec2>(3u, glm::vec2(0.0f));
    mesh.uvSets[1] = std::vector<glm::vec2>(3u, glm::vec2(0.0f));
    mesh.indices = {0u, 1u, 2u};
    return mesh;
}

glm::vec2 polygonCenterXZ(const core::NavPolygon& polygon) {
    glm::vec2 center(0.0f);
    for (const glm::vec2& vertex : polygon.verticesXZ) {
        center += vertex;
    }
    return center / static_cast<float>(polygon.verticesXZ.size());
}

bool containsPoint3(const std::vector<glm::vec3>& points, const glm::vec3& needle, double epsilon = 0.001) {
    return std::any_of(points.begin(), points.end(), [&](const glm::vec3& point) {
        return nearlyEqual(point.x, needle.x, epsilon) &&
            nearlyEqual(point.y, needle.y, epsilon) &&
            nearlyEqual(point.z, needle.z, epsilon);
    });
}

bool nearlyEqualVec3(const glm::vec3& lhs, const glm::vec3& rhs, double epsilon = 0.001) {
    return nearlyEqual(lhs.x, rhs.x, epsilon) &&
        nearlyEqual(lhs.y, rhs.y, epsilon) &&
        nearlyEqual(lhs.z, rhs.z, epsilon);
}

bool pathsEqual(const std::vector<glm::vec3>& lhs, const std::vector<glm::vec3>& rhs, double epsilon = 0.001) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (!nearlyEqualVec3(lhs[index], rhs[index], epsilon)) {
            return false;
        }
    }
    return true;
}

std::vector<core::NavPolygon> makeLinearNavPolygons(
    std::size_t polygonCount,
    float startX = 0.0f,
    float elevationY = 0.0f,
    float minZ = -1.0f,
    float maxZ = 1.0f
) {
    std::vector<core::NavPolygon> polygons{};
    polygons.reserve(polygonCount);
    for (std::size_t index = 0; index < polygonCount; ++index) {
        const float minX = startX + static_cast<float>(index);
        const float maxX = minX + 1.0f;
        polygons.push_back(core::NavPolygon{
            static_cast<int>(index + 1u),
            elevationY,
            {
                glm::vec2(minX, minZ),
                glm::vec2(maxX, minZ),
                glm::vec2(maxX, maxZ),
                glm::vec2(minX, maxZ),
            }
        });
    }
    return polygons;
}

float cross2(const glm::vec2& lhs, const glm::vec2& rhs) {
    return lhs.x * rhs.y - lhs.y * rhs.x;
}

bool pointOnSegmentXZ(const glm::vec2& point, const glm::vec2& a, const glm::vec2& b, double epsilon = 0.001) {
    const glm::vec2 ab = b - a;
    const glm::vec2 ap = point - a;
    if (std::abs(cross2(ab, ap)) > epsilon) {
        return false;
    }
    return glm::dot(point - a, point - b) <= epsilon;
}

bool pointInPolygonXZ(const glm::vec2& point, const std::vector<glm::vec2>& polygon) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1u; i < polygon.size(); j = i++) {
        const glm::vec2& a = polygon[i];
        const glm::vec2& b = polygon[j];
        const bool intersect = ((a.y > point.y) != (b.y > point.y)) &&
            (point.x < ((b.x - a.x) * (point.y - a.y) / ((b.y - a.y) + 1.0e-5f) + a.x));
        if (intersect) {
            inside = !inside;
        }
    }
    return inside;
}

bool pointInOrOnPolygonXZ(const glm::vec2& point, const std::vector<glm::vec2>& polygon) {
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        if (pointOnSegmentXZ(point, polygon[index], polygon[(index + 1u) % polygon.size()])) {
            return true;
        }
    }
    return pointInPolygonXZ(point, polygon);
}

std::optional<std::int64_t> findCounterValue(
    const std::vector<render::FrameCounterRecord>& counters,
    std::string_view name,
    std::string_view group = {}
) {
    const auto it = std::find_if(counters.begin(), counters.end(), [name, group](const render::FrameCounterRecord& counter) {
        return counter.name == name && counter.group == group;
    });
    if (it == counters.end()) {
        return std::nullopt;
    }
    return it->value;
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

bool containsRecordedScopeNamed(const std::vector<core::ProfilerRecordedScope>& scopes, std::string_view name) {
    return std::any_of(scopes.begin(), scopes.end(), [name](const core::ProfilerRecordedScope& scope) {
        return scope.name == name;
    });
}

std::optional<std::uint64_t> findRecordedScopeDurationNs(
    const std::vector<core::ProfilerRecordedScope>& scopes,
    std::string_view name
) {
    const auto it = std::find_if(scopes.begin(), scopes.end(), [name](const core::ProfilerRecordedScope& scope) {
        return scope.name == name;
    });
    if (it == scopes.end() || it->endNs < it->startNs) {
        return std::nullopt;
    }
    return it->endNs - it->startNs;
}

std::int64_t requireCounterValue(
    const std::vector<render::FrameCounterRecord>& counters,
    std::string_view name,
    std::string_view group
) {
    const std::optional<std::int64_t> value = findCounterValue(counters, name, group);
    assert(value.has_value());
    return *value;
}

void waitForNavigationRequestsToDrain(
    core::NavigationSystem& navigation,
    core::World& world,
    const core::NavigationRuntime& runtime,
    int maxAttempts = 2000
) {
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        navigation.applyCompletedPathRequests(world, runtime);
        const auto counters = navigation.profilingCounters();
        if (requireCounterValue(counters, "Pending Path Requests", "Navigation") == 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "Timed out waiting for navigation path requests to complete");
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
    assert(session.navMeshWindowVisible);

    session.showAllWindows();
    assert(session.mainWindowVisible);
    assert(session.mainWindowFocusRequested);
    assert(session.sceneHierarchyVisible);
    assert(session.inspectorWindowVisible);
    assert(session.profilerWindowVisible);
    assert(session.navMeshWindowVisible);
    assert(!session.sceneHierarchyFocusRequested);
    assert(!session.inspectorWindowFocusRequested);
    assert(!session.profilerWindowFocusRequested);
    assert(!session.navMeshWindowFocusRequested);

    session.setToolWindowsVisible(false);
    assert(!session.anyToolWindowVisible());
    assert(session.mainWindowVisible);

    session.ensureToolWindowsVisible();
    assert(session.sceneHierarchyVisible);
    assert(session.inspectorWindowVisible);
    assert(session.profilerWindowVisible);
    assert(session.navMeshWindowVisible);

    session.sceneHierarchyVisible = false;
    session.inspectorWindowVisible = true;
    session.profilerWindowVisible = false;
    session.navMeshWindowVisible = false;
    session.suspendEditorUi();
    assert(!session.mainWindowVisible);
    assert(!session.sceneHierarchyVisible);
    assert(session.inspectorWindowVisible);
    assert(!session.profilerWindowVisible);
    assert(!session.mainWindowFocusRequested);
    assert(!session.sceneHierarchyFocusRequested);
    assert(!session.inspectorWindowFocusRequested);
    assert(!session.profilerWindowFocusRequested);
    assert(!session.navMeshWindowFocusRequested);
    assert(!session.textureBrowserFocusRequested);

    session.openMainWindow();
    assert(session.mainWindowVisible);
    assert(!session.sceneHierarchyVisible);
    assert(session.inspectorWindowVisible);
    assert(!session.profilerWindowVisible);
    assert(!session.navMeshWindowVisible);
}

void testEditorSessionImGuiSettingsRoundTrip() {
    core::EditorSession session;
    session.sceneHierarchyVisible = false;
    session.inspectorWindowVisible = false;
    session.profilerWindowVisible = true;
    session.navMeshWindowVisible = false;
    session.navMeshOverlayVisible = false;
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
    assert(ini.find("NavMeshWindowVisible=0") != std::string::npos);
    assert(ini.find("NavMeshOverlayVisible=0") != std::string::npos);
    assert(ini.find("ProfilerFollowLatest=0") != std::string::npos);
    ImGui::DestroyContext();

    core::EditorSession restored;
    restored.sceneHierarchyVisible = true;
    restored.inspectorWindowVisible = true;
    restored.profilerWindowVisible = false;
    restored.navMeshWindowVisible = true;
    restored.navMeshOverlayVisible = true;
    restored.profilerFollowLatest = true;

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    core::registerEditorSessionImGuiSettings(restored);
    ImGui::LoadIniSettingsFromMemory(ini.c_str(), ini.size());
    assert(!restored.sceneHierarchyVisible);
    assert(!restored.inspectorWindowVisible);
    assert(restored.profilerWindowVisible);
    assert(!restored.navMeshWindowVisible);
    assert(!restored.navMeshOverlayVisible);
    assert(!restored.profilerFollowLatest);
    ImGui::DestroyContext();
}

void testNavigationAssetRoundTrip() {
    core::NavMeshAsset asset{};
    asset.sourceTagOverrides.push_back(core::NavSourceTagOverride{"Root/Ground", core::NavSourceTag::Walkable});
    asset.polygons.push_back(core::NavPolygon{
        1,
        0.0f,
        {
            glm::vec2(-1.0f, -1.0f),
            glm::vec2(1.0f, -1.0f),
            glm::vec2(1.0f, 1.0f),
            glm::vec2(-1.0f, 1.0f),
        }
    });
    asset.links.push_back(core::NavLink{
        7,
        1,
        2,
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 3.0f, 0.0f),
        true
    });

    const std::string serialized = core::serializeNavMeshAsset(asset);
    core::NavMeshAsset parsed{};
    std::string error{};
    assert(core::parseNavMeshAsset(serialized, parsed, &error));
    assert(error.empty());
    assert(parsed.version == 1);
    assert(parsed.sourceTagOverrides.size() == 1u);
    assert(parsed.sourceTagOverrides[0].stableId == "Root/Ground");
    assert(parsed.sourceTagOverrides[0].tag == core::NavSourceTag::Walkable);
    assert(parsed.polygons.size() == 1u);
    assert(parsed.polygons[0].id == 1);
    assert(parsed.polygons[0].verticesXZ.size() == 4u);
    assert(parsed.links.size() == 1u);
    assert(parsed.links[0].id == 7);
    assert(parsed.links[0].bidirectional);
}

void testNavigationBakeBuildsMultiLevelPolygonsAndBlocksOnlyOverlappingLayers() {
    core::World world;
    core::TransformSystem transformSystem;
    core::TaskScheduler scheduler = makeScheduler();

    const core::EntityId ground = world.createEntity();
    world.transforms.emplace(ground, core::TransformComponent{});
    world.navSources.emplace(ground, core::NavSourceComponent{"Ground", core::NavSourceTag::Walkable, core::NavSourceTag::Walkable});
    world.navSourceGeometry.emplace(ground, core::NavSourceGeometryComponent{std::make_shared<render::Mesh>(makeHorizontalQuadMesh(-5.0f, 5.0f, -5.0f, 5.0f, 0.0f))});
    world.markTransformsDirty(ground);

    const core::EntityId blocker = world.createEntity();
    world.transforms.emplace(blocker, core::TransformComponent{});
    world.navSources.emplace(blocker, core::NavSourceComponent{"Blocker", core::NavSourceTag::Blocking, core::NavSourceTag::Blocking});
    world.navSourceGeometry.emplace(blocker, core::NavSourceGeometryComponent{std::make_shared<render::Mesh>(makeBoxVertexMesh(glm::vec3(-1.0f, 0.0f, -1.0f), glm::vec3(1.0f, 1.5f, 1.0f)))});
    world.markTransformsDirty(blocker);

    const core::EntityId upper = world.createEntity();
    world.transforms.emplace(upper, core::TransformComponent{});
    world.navSources.emplace(upper, core::NavSourceComponent{"Upper", core::NavSourceTag::Walkable, core::NavSourceTag::Walkable});
    world.navSourceGeometry.emplace(upper, core::NavSourceGeometryComponent{std::make_shared<render::Mesh>(makeHorizontalQuadMesh(-1.0f, 1.0f, -1.0f, 1.0f, 3.0f))});
    world.markTransformsDirty(upper);

    const core::EntityId sloped = world.createEntity();
    world.transforms.emplace(sloped, core::TransformComponent{});
    world.navSources.emplace(sloped, core::NavSourceComponent{"Slope", core::NavSourceTag::Walkable, core::NavSourceTag::Walkable});
    world.navSourceGeometry.emplace(sloped, core::NavSourceGeometryComponent{std::make_shared<render::Mesh>(makeSlopedTriangleMesh())});
    world.markTransformsDirty(sloped);

    transformSystem.update(world, scheduler, false);

    core::NavigationRuntime runtime{};
    core::NavigationSystem navigation;
    std::string error{};
    assert(navigation.generateFromTags(world, runtime, &error));
    assert(error.empty());
    assert(!runtime.asset.polygons.empty());

    bool hasGroundLayer = false;
    bool hasUpperLayer = false;
    bool groundInsideBlocker = false;
    bool upperInsideBlocker = false;
    bool foundSlopeLayer = false;
    for (const core::NavPolygon& polygon : runtime.asset.polygons) {
        const glm::vec2 center = polygonCenterXZ(polygon);
        if (nearlyEqual(polygon.elevationY, 0.0f, 0.15)) {
            hasGroundLayer = true;
            if (center.x > -1.0f && center.x < 1.0f && center.y > -1.0f && center.y < 1.0f) {
                groundInsideBlocker = true;
            }
        }
        if (nearlyEqual(polygon.elevationY, 3.0f, 0.15)) {
            hasUpperLayer = true;
            if (center.x > -1.0f && center.x < 1.0f && center.y > -1.0f && center.y < 1.0f) {
                upperInsideBlocker = true;
            }
        }
        if (polygon.elevationY > 0.2f && polygon.elevationY < 2.0f) {
            foundSlopeLayer = true;
        }
    }

    assert(hasGroundLayer);
    assert(hasUpperLayer);
    assert(!groundInsideBlocker);
    assert(upperInsideBlocker);
    assert(!foundSlopeLayer);
}

void testNavigationPathfindingUsesSameLayerFunnelAndExplicitLinks() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(-1.0f, -1.0f), glm::vec2(0.0f, -1.0f), glm::vec2(0.0f, 1.0f), glm::vec2(-1.0f, 1.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(0.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f)}},
        core::NavPolygon{3, 0.0f, {glm::vec2(1.0f, -1.0f), glm::vec2(2.0f, -1.0f), glm::vec2(2.0f, 1.0f), glm::vec2(1.0f, 1.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{glm::vec3(-0.75f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, glm::vec3(1.75f, 0.0f, 0.0f)));
    const core::NavAgentComponent& straightAgent = world.navAgents.get(agentEntity);
    assert(straightAgent.pathCorners.size() == 1u);
    assert(nearlyEqual(straightAgent.pathCorners.back().x, 1.75, 0.001));

    runtime.asset.polygons = {
        core::NavPolygon{4, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(2.0f, 0.0f), glm::vec2(2.0f, 2.0f), glm::vec2(0.0f, 2.0f)}},
        core::NavPolygon{5, 0.0f, {glm::vec2(1.0f, 0.0f), glm::vec2(3.0f, 0.0f), glm::vec2(3.0f, 2.0f), glm::vec2(1.0f, 2.0f)}},
    };
    runtime.asset.links.clear();
    assert(navigation.rebuildRuntime(runtime));
    assert(!runtime.bakedCells.empty());

    core::NavAgentComponent& overlapAgent = world.navAgents.get(agentEntity);
    overlapAgent.pathCorners.clear();
    overlapAgent.destination.reset();
    overlapAgent.moving = false;
    world.transforms.get(agentEntity).position = glm::vec3(2.5f, 0.0f, 1.0f);

    assert(navigation.setAgentDestination(world, runtime, agentEntity, glm::vec3(1.5f, 0.0f, 1.0f)));
    assert(!overlapAgent.pathCorners.empty());
    assert(nearlyEqual(overlapAgent.pathCorners.back().x, 1.5, 0.001));
    assert(nearlyEqual(overlapAgent.pathCorners.back().z, 1.0, 0.001));

    runtime.asset.polygons = {
        core::NavPolygon{6, 0.0f, {glm::vec2(1.0f, 0.0f), glm::vec2(3.0f, 0.0f), glm::vec2(3.0f, 2.0f), glm::vec2(1.0f, 2.0f)}},
        core::NavPolygon{7, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(2.0f, 0.0f), glm::vec2(2.0f, 2.0f), glm::vec2(0.0f, 2.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));
    overlapAgent.pathCorners.clear();
    overlapAgent.destination.reset();
    overlapAgent.moving = false;
    world.transforms.get(agentEntity).position = glm::vec3(0.25f, 0.0f, 1.0f);

    assert(navigation.setAgentDestination(world, runtime, agentEntity, glm::vec3(1.5f, 0.0f, 1.0f)));
    assert(overlapAgent.pathCorners.size() == 1u);
    assert(nearlyEqual(overlapAgent.pathCorners.front().x, 1.5, 0.001));
    assert(nearlyEqual(overlapAgent.pathCorners.front().z, 1.0, 0.001));

    overlapAgent.pathCorners.clear();
    overlapAgent.destination.reset();
    overlapAgent.moving = false;
    world.transforms.get(agentEntity).position = glm::vec3(0.25f, 0.0f, 1.0f);
    assert(navigation.setAgentDestination(world, runtime, agentEntity, glm::vec3(2.75f, 0.0f, 1.0f)));
    assert(overlapAgent.pathCorners.size() == 1u);
    assert(nearlyEqual(overlapAgent.pathCorners.front().x, 2.75, 0.001));
    assert(nearlyEqual(overlapAgent.pathCorners.front().z, 1.0, 0.001));

    runtime.asset.polygons = {
        core::NavPolygon{10, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f)}},
        core::NavPolygon{11, 3.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f)}},
    };
    runtime.asset.links = {
        core::NavLink{3, 10, 11, glm::vec3(0.5f, 0.0f, 0.5f), glm::vec3(0.5f, 3.0f, 0.5f), true}
    };
    assert(navigation.rebuildRuntime(runtime));
    assert(!runtime.bakedCells.empty());

    core::NavAgentComponent& linkedAgent = world.navAgents.get(agentEntity);
    linkedAgent.pathCorners.clear();
    linkedAgent.destination.reset();
    linkedAgent.moving = false;
    world.transforms.get(agentEntity).position = glm::vec3(0.25f, 0.0f, 0.25f);

    assert(navigation.setAgentDestination(world, runtime, agentEntity, glm::vec3(0.75f, 3.0f, 0.75f)));
    assert(linkedAgent.pathCorners.size() >= 3u);
    assert(containsPoint3(linkedAgent.pathCorners, glm::vec3(0.5f, 0.0f, 0.5f)));
    assert(containsPoint3(linkedAgent.pathCorners, glm::vec3(0.5f, 3.0f, 0.5f)));
}

bool pointInsideWalkableUnion(
    const glm::vec3& point,
    const std::vector<core::NavPolygon>& polygons
) {
    const glm::vec2 pointXZ(point.x, point.z);
    for (const core::NavPolygon& polygon : polygons) {
        if (std::abs(point.y - polygon.elevationY) > 0.1f) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, polygon.verticesXZ)) {
            return true;
        }
    }
    return false;
}

bool segmentInsidePolygons(
    const glm::vec3& from,
    const glm::vec3& to,
    const std::vector<core::NavPolygon>& polygons
) {
    constexpr int kSamples = 20;
    for (int i = 0; i <= kSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSamples);
        const glm::vec3 point = from + t * (to - from);
        if (!pointInsideWalkableUnion(point, polygons)) {
            return false;
        }
    }
    return true;
}

double pathLength(const glm::vec3& start, const std::vector<glm::vec3>& corners) {
    double total = 0.0;
    glm::vec3 previous = start;
    for (const glm::vec3& corner : corners) {
        total += glm::distance(previous, corner);
        previous = corner;
    }
    return total;
}

void testNavigationOverlapCorridorConstraint() {
    // Two polygons form an L-shape connected only through a small overlap.
    // A = tall rectangle on the left, B = wide rectangle on the top-right.
    // The straight line from bottom-A to right-B would exit the walkable area,
    // so the funnel must route through the overlap region.
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(2.0f, 0.0f), glm::vec2(2.0f, 4.0f), glm::vec2(0.0f, 4.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(1.0f, 3.0f), glm::vec2(5.0f, 3.0f), glm::vec2(5.0f, 5.0f), glm::vec2(1.0f, 5.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));
    assert(!runtime.bakedCells.empty());

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{glm::vec3(0.5f, 0.0f, 0.5f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, glm::vec3(4.0f, 0.0f, 4.0f)));
    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(!agent.pathCorners.empty());

    // Verify every path segment stays inside at least one polygon.
    glm::vec3 previous = glm::vec3(0.5f, 0.0f, 0.5f);
    for (const glm::vec3& corner : agent.pathCorners) {
        assert(segmentInsidePolygons(previous, corner, runtime.asset.polygons));
        previous = corner;
    }
}

void testNavigationConcavePolygonStaysInsideWalkableSurface() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{
            1,
            0.0f,
            {
                glm::vec2(0.0f, 0.0f),
                glm::vec2(4.0f, 0.0f),
                glm::vec2(4.0f, 1.0f),
                glm::vec2(1.0f, 1.0f),
                glm::vec2(1.0f, 4.0f),
                glm::vec2(0.0f, 4.0f),
            }
        }
    };
    assert(navigation.rebuildRuntime(runtime));
    assert(runtime.bakedCells.size() > 1u);

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    const glm::vec3 start(0.5f, 0.0f, 3.5f);
    const glm::vec3 destination(3.5f, 0.0f, 0.5f);
    world.transforms.emplace(agentEntity, core::TransformComponent{start, glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(agent.pathCorners.size() >= 2u);

    glm::vec3 previous = start;
    for (const glm::vec3& corner : agent.pathCorners) {
        assert(segmentInsidePolygons(previous, corner, runtime.asset.polygons));
        previous = corner;
    }
}

void testNavigationOverlapPrefersShortestStraightCorridor() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(4.0f, 0.0f), glm::vec2(4.0f, 2.0f), glm::vec2(0.0f, 2.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(2.0f, -1.0f), glm::vec2(6.0f, -1.0f), glm::vec2(6.0f, 1.0f), glm::vec2(2.0f, 1.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));
    assert(!runtime.bakedCells.empty());

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    const glm::vec3 start(0.5f, 0.0f, 1.5f);
    const glm::vec3 destination(5.5f, 0.0f, -0.5f);
    world.transforms.emplace(agentEntity, core::TransformComponent{start, glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(agent.pathCorners.size() == 1u);
    assert(segmentInsidePolygons(start, agent.pathCorners.front(), runtime.asset.polygons));
    assert(nearlyEqual(pathLength(start, agent.pathCorners), glm::distance(start, destination), 0.001));
}

void testNavigationOverlapCandidateSelectionUsesMultiplyCoveredStartCell() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(4.0f, 0.0f), glm::vec2(4.0f, 2.0f), glm::vec2(0.0f, 2.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(2.0f, 0.0f), glm::vec2(6.0f, 0.0f), glm::vec2(6.0f, 2.0f), glm::vec2(2.0f, 2.0f)}},
        core::NavPolygon{3, 0.0f, {glm::vec2(0.0f, 2.0f), glm::vec2(4.0f, 2.0f), glm::vec2(4.0f, 4.0f), glm::vec2(0.0f, 4.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));
    assert(!runtime.bakedCells.empty());

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    const glm::vec3 start(3.0f, 0.0f, 1.0f);
    const glm::vec3 destination(1.0f, 0.0f, 3.0f);
    world.transforms.emplace(agentEntity, core::TransformComponent{start, glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(agent.pathCorners.size() == 1u);
    assert(segmentInsidePolygons(start, agent.pathCorners.front(), runtime.asset.polygons));
    assert(nearlyEqual(pathLength(start, agent.pathCorners), glm::distance(start, destination), 0.001));
}

void testNavigationRejectsSelfIntersectingPolygon() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{
            1,
            0.0f,
            {
                glm::vec2(0.0f, 0.0f),
                glm::vec2(2.0f, 2.0f),
                glm::vec2(0.0f, 2.0f),
                glm::vec2(2.0f, 0.0f),
            }
        }
    };

    std::string error{};
    assert(!navigation.rebuildRuntime(runtime, &error));
    assert(error.find("Self-intersecting polygon geometry") != std::string::npos);
}

void testNavigationHitTestFindsProjectedNavPolygon() {
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(-2.0f, -2.0f), glm::vec2(2.0f, -2.0f), glm::vec2(2.0f, 2.0f), glm::vec2(-2.0f, 2.0f)}}
    };
    core::NavigationSystem navigation;
    assert(navigation.rebuildRuntime(runtime));

    const render::CameraMatrices camera = core::computeCameraMatrices(core::CameraState{}, 100, 100);
    const std::optional<core::NavHitResult> hit = navigation.hitTest(runtime, camera, 100, 100, 50, 50);
    assert(hit.has_value());
    assert(hit->polygonId == 1);
    assert(nearlyEqual(hit->position.y, 0.0, 0.01));
    assert(std::abs(hit->position.x) < 0.25f);
    assert(std::abs(hit->position.z) < 0.25f);
}

void testNavigationAgentMovementRotatesAndRequestsWalkThenIdle() {
    auto model = std::make_shared<render::GltfModelData>();
    assert(render::loadGltfModel(assetPath("src/render/models/Adventurer.glb").string(), *model));

    core::World world;
    const core::EntityId entity = world.createEntity();
    world.transforms.emplace(entity, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.animatedModels.emplace(entity, core::AnimatedModelComponent{model});
    world.navAgents.emplace(entity, core::NavAgentComponent{2.0f, 360.0f, 0.05f});
    world.locomotion.emplace(entity, core::LocomotionComponent{
        render::findDefaultAnimationClipIndex(*model),
        findClipIndexContaining(*model, "Walk")
    });

    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(-2.0f, -2.0f), glm::vec2(4.0f, -2.0f), glm::vec2(4.0f, 2.0f), glm::vec2(-2.0f, 2.0f)}}
    };
    core::NavigationSystem navigation;
    assert(navigation.rebuildRuntime(runtime));
    assert(navigation.setAgentDestination(world, runtime, entity, glm::vec3(2.0f, 0.0f, 0.0f)));

    navigation.updateAgents(world, runtime, core::TimeContext{0.0f, 0.25f});
    const core::TransformComponent& movedTransform = world.transforms.get(entity);
    const core::AnimatedModelComponent& movedAnimation = world.animatedModels.get(entity);
    assert(movedTransform.position.x > 0.0f);
    assert(std::abs(movedTransform.rotationDeg.y) > 0.1f);
    assert(movedAnimation.requestedClip == world.locomotion.get(entity).walkClip);

    for (int step = 0; step < 12; ++step) {
        navigation.updateAgents(world, runtime, core::TimeContext{0.0f, 0.25f});
    }
    navigation.updateAgents(world, runtime, core::TimeContext{0.0f, 0.1f});

    const core::NavAgentComponent& finalAgent = world.navAgents.get(entity);
    const core::AnimatedModelComponent& finalAnimation = world.animatedModels.get(entity);
    assert(!finalAgent.moving);
    assert(!finalAgent.destination.has_value());
    assert(finalAnimation.requestedClip == world.locomotion.get(entity).idleClip);
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

void testJointInfluenceBoundsBuildsSingleJointSection() {
    render::GltfMeshSection section{};
    section.skinIndex = 0;
    section.mesh.positions = {
        glm::vec3(-1.0f, -2.0f, 0.5f),
        glm::vec3(3.0f, 4.0f, 1.5f),
    };
    section.mesh.jointIndices = {
        glm::uvec4(3u, 0u, 0u, 0u),
        glm::uvec4(3u, 1u, 0u, 0u),
    };
    section.mesh.jointWeights = {
        glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
        glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
    };

    render::buildJointInfluenceBounds(section);

    assert(section.jointInfluenceBounds.size() == 1u);
    assert(section.jointInfluenceBounds[0].jointIndex == 3);
    assert(nearlyEqual(section.jointInfluenceBounds[0].localBounds.min.x, -1.0, 0.0001));
    assert(nearlyEqual(section.jointInfluenceBounds[0].localBounds.min.y, -2.0, 0.0001));
    assert(nearlyEqual(section.jointInfluenceBounds[0].localBounds.max.x, 3.0, 0.0001));
    assert(nearlyEqual(section.jointInfluenceBounds[0].localBounds.max.y, 4.0, 0.0001));
    assert(nearlyEqual(section.jointInfluenceBounds[0].localBounds.max.z, 1.5, 0.0001));
}

void testJointInfluenceBoundsBuildsMixedWeightSection() {
    render::GltfMeshSection section{};
    section.skinIndex = 0;
    section.mesh.positions = {
        glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 2.0f, 0.0f),
        glm::vec3(3.0f, -1.0f, 1.0f),
    };
    section.mesh.jointIndices = {
        glm::uvec4(0u, 0u, 0u, 0u),
        glm::uvec4(0u, 2u, 0u, 0u),
        glm::uvec4(2u, 0u, 0u, 0u),
    };
    section.mesh.jointWeights = {
        glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
        glm::vec4(0.25f, 0.75f, 0.0f, 0.0f),
        glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
    };

    render::buildJointInfluenceBounds(section);

    assert(section.jointInfluenceBounds.size() == 2u);
    const auto joint0 = std::find_if(
        section.jointInfluenceBounds.begin(),
        section.jointInfluenceBounds.end(),
        [](const render::JointInfluenceBounds& bounds) { return bounds.jointIndex == 0; }
    );
    const auto joint2 = std::find_if(
        section.jointInfluenceBounds.begin(),
        section.jointInfluenceBounds.end(),
        [](const render::JointInfluenceBounds& bounds) { return bounds.jointIndex == 2; }
    );
    assert(joint0 != section.jointInfluenceBounds.end());
    assert(joint2 != section.jointInfluenceBounds.end());

    assert(nearlyEqual(joint0->localBounds.min.x, -1.0, 0.0001));
    assert(nearlyEqual(joint0->localBounds.max.x, 0.0, 0.0001));
    assert(nearlyEqual(joint0->localBounds.max.y, 2.0, 0.0001));
    assert(nearlyEqual(joint2->localBounds.min.x, 0.0, 0.0001));
    assert(nearlyEqual(joint2->localBounds.min.y, -1.0, 0.0001));
    assert(nearlyEqual(joint2->localBounds.max.x, 3.0, 0.0001));
    assert(nearlyEqual(joint2->localBounds.max.y, 2.0, 0.0001));
    assert(nearlyEqual(joint2->localBounds.max.z, 1.0, 0.0001));
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

void testRenderExtractionConservativeSkinnedBoundsContainExactBounds() {
    auto model = std::make_shared<render::GltfModelData>();
    model->skins.emplace_back();

    render::GltfMeshSection section{};
    section.name = "Conservative Bounds Section";
    section.nodeIndex = 0;
    section.skinIndex = 0;
    section.mesh.positions = {glm::vec3(0.0f, 0.0f, 0.0f)};
    section.mesh.jointIndices = {glm::uvec4(0u, 1u, 0u, 0u)};
    section.mesh.jointWeights = {glm::vec4(0.5f, 0.5f, 0.0f, 0.0f)};
    render::buildJointInfluenceBounds(section);
    model->sections.push_back(std::move(section));

    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::MaterialLibrary materials;
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::RenderExtractionSystem extraction;

    auto material = std::make_shared<render::Material>();
    const core::MaterialHandle materialHandle = materials.add(material);

    const std::vector<glm::mat4> skinMatrices{
        glm::translate(glm::mat4(1.0f), glm::vec3(-2.0f, 0.0f, 0.0f)),
        glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f)),
    };
    const render::Bounds3 exactBounds = exactSkinnedWorldBounds(
        model->sections[0].mesh,
        skinMatrices,
        glm::mat4(1.0f)
    );

    const core::EntityId root = world.createEntity();
    world.transforms.emplace(root, core::TransformComponent{});
    core::AnimatedModelComponent animated{model};
    animated.skinJointMatrices = {skinMatrices};
    world.animatedModels.emplace(root, std::move(animated));

    const core::EntityId sectionEntity = world.createEntity();
    world.parents.emplace(sectionEntity, core::ParentComponent{root});
    world.transforms.emplace(sectionEntity, core::TransformComponent{});
    world.bounds.emplace(sectionEntity, core::BoundsComponent{
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)}
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

    core::FrameSceneData frame;
    selection.set(sectionEntity);
    extraction.extract(world, materials, selection, frame, scheduler, false);

    assert(frame.renderables.size() == 1u);
    assert(boundsContain(frame.renderables[0].worldBounds, exactBounds));
    assert(frame.renderables[0].worldBounds.min.x < exactBounds.min.x - 0.0001);
    assert(frame.renderables[0].worldBounds.max.x > exactBounds.max.x + 0.0001);
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
        true,
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
    assert(scene.objects[0].hasWorldBounds);
    assert(scene.objects[0].frustumVisible);
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

void testCameraFrustumCullingUpdatesVisibilityAndStats() {
    core::TaskScheduler scheduler = makeScheduler();
    core::FrameSceneData frame;
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{1, 1},
        render::MeshHandle{0},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        true,
        glm::mat4(1.0f),
        true
    });
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{2, 1},
        render::MeshHandle{1},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(1.5f, -0.25f, -2.0f), glm::vec3(2.0f, 0.25f, -1.5f)},
        true,
        glm::mat4(1.0f),
        true
    });
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{3, 1},
        render::MeshHandle{2},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        true,
        glm::mat4(1.0f),
        false
    });
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{4, 1},
        render::MeshHandle{3},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{},
        false,
        glm::mat4(1.0f),
        true
    });

    render::RenderSceneView scene = render::buildRenderSceneView(
        frame,
        std::vector<const render::MeshBuffer*>{nullptr, nullptr, nullptr, nullptr},
        scheduler
    );
    render::applyCameraFrustumCulling(scene, makeOrthoCamera());

    assert(scene.objects.size() == 4u);
    assert(scene.objects[0].frustumVisible);
    assert(!scene.objects[1].frustumVisible);
    assert(scene.objects[2].frustumVisible);
    assert(scene.objects[3].frustumVisible);
    assert(scene.frustumCullStats.totalRenderables == 4u);
    assert(scene.frustumCullStats.visibilityHidden == 1u);
    assert(scene.frustumCullStats.boundsTested == 2u);
    assert(scene.frustumCullStats.frustumPassed == 1u);
    assert(scene.frustumCullStats.frustumCulled == 1u);
    assert(scene.frustumCullStats.noBoundsBypass == 1u);
    assert(scene.frustumCullStats.mainPassVisible == 2u);
}

void testCameraFrustumCullingRejectsFarPlaneBounds() {
    core::TaskScheduler scheduler = makeScheduler();
    core::FrameSceneData frame;
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{5, 1},
        render::MeshHandle{0},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f, -0.25f, -12.0f), glm::vec3(0.25f, 0.25f, -11.5f)},
        true,
        glm::mat4(1.0f),
        true
    });

    render::RenderSceneView scene = render::buildRenderSceneView(
        frame,
        std::vector<const render::MeshBuffer*>{nullptr},
        scheduler
    );
    render::applyCameraFrustumCulling(scene, makeOrthoCamera());

    assert(scene.frustumCullStats.boundsTested == 1u);
    assert(scene.frustumCullStats.frustumCulled == 1u);
    assert(!scene.objects[0].frustumVisible);
}

void testOcclusionCullingUsesLastKnownResultsAndWarmup() {
    core::TaskScheduler scheduler = makeScheduler();
    core::FrameSceneData frame;
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{10, 1},
        render::MeshHandle{0},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        true,
        glm::mat4(1.0f),
        true
    });
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{11, 1},
        render::MeshHandle{1},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(0.1f, 0.1f, -2.0f), glm::vec3(0.6f, 0.6f, -1.5f)},
        true,
        glm::mat4(1.0f),
        true
    });
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{12, 1},
        render::MeshHandle{2},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{},
        false,
        glm::mat4(1.0f),
        true
    });
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{13, 1},
        render::MeshHandle{3},
        {},
        {},
        render::RenderLayer::Ground,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        true,
        glm::mat4(1.0f),
        true
    });

    render::RenderSceneView scene = render::buildRenderSceneView(
        frame,
        std::vector<const render::MeshBuffer*>{nullptr, nullptr, nullptr, nullptr},
        scheduler
    );
    render::applyCameraFrustumCulling(scene, makeOrthoCamera());
    std::unordered_map<core::EntityId, render::OcclusionCullCacheState> cache{
        {core::EntityId{11, 1}, render::OcclusionCullCacheState{true, true, false, 1}},
    };
    render::applyLastKnownOcclusionVisibility(scene, cache);

    assert(scene.objects[0].occlusionVisible);
    assert(scene.objects[1].occlusionVisible);
    assert(scene.objects[2].occlusionVisible);
    assert(scene.objects[3].occlusionVisible);
    assert(scene.occlusionCullStats.candidates == 2u);
    assert(scene.occlusionCullStats.visible == 2u);
    assert(scene.occlusionCullStats.occluded == 0u);
    assert(scene.occlusionCullStats.warmupVisible == 1u);
    assert(scene.occlusionCullStats.pendingReused == 1u);
    assert(scene.occlusionCullStats.noBoundsBypass == 1u);
}

void testOcclusionCullingHidesAfterConsecutiveOccludedResults() {
    core::TaskScheduler scheduler = makeScheduler();
    core::FrameSceneData frame;
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{15, 1},
        render::MeshHandle{0},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f, -0.25f, -2.0f), glm::vec3(0.25f, 0.25f, -1.5f)},
        true,
        glm::mat4(1.0f),
        true
    });

    render::RenderSceneView scene = render::buildRenderSceneView(
        frame,
        std::vector<const render::MeshBuffer*>{nullptr},
        scheduler
    );
    render::applyCameraFrustumCulling(scene, makeOrthoCamera());
    std::unordered_map<core::EntityId, render::OcclusionCullCacheState> cache{
        {core::EntityId{15, 1}, render::OcclusionCullCacheState{false, true, false, 2}},
    };
    render::applyLastKnownOcclusionVisibility(scene, cache);

    assert(!scene.objects[0].occlusionVisible);
    assert(scene.occlusionCullStats.candidates == 1u);
    assert(scene.occlusionCullStats.visible == 0u);
    assert(scene.occlusionCullStats.occluded == 1u);
}

void testOcclusionCullingTreatsFrustumRejectedObjectsAsNonCandidates() {
    core::TaskScheduler scheduler = makeScheduler();
    core::FrameSceneData frame;
    frame.renderables.push_back(core::FrameRenderable{
        core::EntityId{14, 1},
        render::MeshHandle{0},
        {},
        {},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(2.0f, 2.0f, -2.0f), glm::vec3(2.5f, 2.5f, -1.5f)},
        true,
        glm::mat4(1.0f),
        true
    });

    render::RenderSceneView scene = render::buildRenderSceneView(
        frame,
        std::vector<const render::MeshBuffer*>{nullptr},
        scheduler
    );
    render::applyCameraFrustumCulling(scene, makeOrthoCamera());
    render::applyLastKnownOcclusionVisibility(scene, {});

    assert(!scene.objects[0].frustumVisible);
    assert(scene.objects[0].occlusionVisible);
    assert(scene.occlusionCullStats.candidates == 0u);
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
        assert(frame.renderables[0].hasWorldBounds);
        assert(frame.renderables[1].hasWorldBounds);
        assert(scene.objects[0].hasWorldBounds);
        assert(scene.objects[1].hasWorldBounds);
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
        true,
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

    render::CameraMatrices camera = makeOrthoCamera(-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 10.0f);

    core::PickingSystem picking;
    const std::optional<core::EntityId> withLights = picking.pick(frame, camera, 100, 100, 50, 50, true);
    const std::optional<core::EntityId> withoutLights = picking.pick(frame, camera, 100, 100, 50, 50, false);

    assert(withLights.has_value());
    assert(withLights->index == 2u);
    assert(withoutLights.has_value());
    assert(withoutLights->index == 1u);
}

void testOverlayWorkSkipsAllOverlayWorkWhenDisabled() {
    render::RenderSceneView scene{};
    scene.selection.kind = render::RenderSelectionKind::Renderable;
    scene.selection.hasWorldBounds = true;
    scene.selection.worldBounds = render::Bounds3{glm::vec3(-1.0f), glm::vec3(1.0f)};
    scene.selectionSkeleton.showOverlay = true;
    scene.selectionSkeleton.parentIndices = {-1, 0};
    scene.selectionSkeleton.jointWorldPositions = {glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)};
    scene.lights.push_back(core::FrameLight{core::EntityId{1, 1}, render::LightType::Point, glm::vec3(0.0f, 0.0f, -2.0f)});
    scene.selection.index = 0;

    render::RenderLightPipeline::FrameState lights{};
    lights.activeLightIndices = {0};
    lights.debugLights.push_back(render::ActiveLightDebug{glm::vec3(0.0f), 1.0f, glm::vec3(1.0f), 0.0f, glm::vec3(0.0f, 0.0f, -1.0f), render::LightType::Point});

    render::CameraMatrices camera{};
    camera.projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 10.0f);
    camera.invProjection = glm::inverse(camera.projection);
    camera.view = glm::mat4(1.0f);

    const render::RenderFrameOptions options{render::DebugView::Final, 0, false, false};
    const render::detail::OverlayWork work = render::detail::buildOverlayWork(scene, lights, camera, options);

    assert(!work.hasWork());
}

void testOverlayWorkCullsIconsAndBatchesSelectedLights() {
    render::RenderSceneView scene{};
    scene.selection.kind = render::RenderSelectionKind::Light;
    scene.selection.index = 1;
    scene.lights = {
        core::FrameLight{core::EntityId{1, 1}, render::LightType::Point, glm::vec3(0.0f, 0.0f, -2.0f)},
        core::FrameLight{core::EntityId{2, 1}, render::LightType::Spot, glm::vec3(0.3f, 0.0f, -2.0f)},
        core::FrameLight{core::EntityId{3, 1}, render::LightType::Point, glm::vec3(5.0f, 0.0f, -2.0f)},
        core::FrameLight{core::EntityId{4, 1}, render::LightType::Spot, glm::vec3(0.0f, 0.0f, 2.0f)},
    };

    render::RenderLightPipeline::FrameState lights{};
    render::CameraMatrices camera{};
    camera.projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 10.0f);
    camera.invProjection = glm::inverse(camera.projection);
    camera.view = glm::mat4(1.0f);

    const render::RenderFrameOptions options{render::DebugView::Final, 0, false, true};
    const render::detail::OverlayWork work = render::detail::buildOverlayWork(scene, lights, camera, options);

    assert(work.hasWork());
    assert(work.iconBatches[0].kind == render::detail::OverlayIconBatchKind::UnselectedPoint);
    assert(work.iconBatches[0].clipCenters.size() == 1u);
    assert(nearlyEqual(work.iconBatches[0].opacity, 0.5));
    assert(work.iconBatches[1].kind == render::detail::OverlayIconBatchKind::UnselectedSpot);
    assert(work.iconBatches[1].clipCenters.empty());
    assert(work.iconBatches[2].kind == render::detail::OverlayIconBatchKind::SelectedPoint);
    assert(work.iconBatches[2].clipCenters.empty());
    assert(work.iconBatches[3].kind == render::detail::OverlayIconBatchKind::SelectedSpot);
    assert(work.iconBatches[3].clipCenters.size() == 1u);
    assert(nearlyEqual(work.iconBatches[3].opacity, 1.0));
}

void testOverlayWorkKeepsSelectedLightDebugWhenLightDebugIsOff() {
    render::RenderSceneView scene{};
    scene.selection.kind = render::RenderSelectionKind::Light;
    scene.selection.index = 1;
    scene.lights = {
        core::FrameLight{core::EntityId{1, 1}, render::LightType::Point, glm::vec3(0.0f, 0.0f, -2.0f)},
        core::FrameLight{core::EntityId{2, 1}, render::LightType::Spot, glm::vec3(0.0f, 0.0f, -3.0f)},
    };

    render::RenderLightPipeline::FrameState lights{};
    lights.activeLightIndices = {0, 1};
    lights.debugLights = {
        render::ActiveLightDebug{glm::vec3(0.0f), 1.0f, glm::vec3(1.0f), 0.0f, glm::vec3(0.0f, 0.0f, -1.0f), render::LightType::Point},
        render::ActiveLightDebug{glm::vec3(1.0f), 2.0f, glm::vec3(1.0f), 25.0f, glm::vec3(0.0f, 0.0f, -1.0f), render::LightType::Spot},
    };

    render::CameraMatrices camera{};
    camera.projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 10.0f);
    camera.invProjection = glm::inverse(camera.projection);
    camera.view = glm::mat4(1.0f);

    const render::RenderFrameOptions options{render::DebugView::Final, 0, false, true};
    const render::detail::OverlayWork work = render::detail::buildOverlayWork(scene, lights, camera, options);

    assert(work.hasWork());
    assert(work.debugLightIndices.size() == 1u);
    assert(work.debugLightIndices[0] == 1);
    assert(!work.drawDirectionalMarker);
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
    assert(snapshots.front()->startNs > 0u);
    assert(snapshots.front()->endNs >= snapshots.front()->startNs);
    assert(snapshots.back()->startNs > snapshots.front()->startNs);
    assert(snapshots.back()->endNs >= snapshots.back()->startNs);

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

void testNavigationPathfindingEmitsProfilerScope() {
    core::ProfilerConfig config{};
    config.maxFrames = 4u;
    config.maxCpuScopesPerFrame = 32u;
    core::ProfilerService profiler(config);

    core::NavigationSystem navigation;
    navigation.setProfiler(&profiler);

    core::NavigationRuntime runtime{};
    constexpr std::size_t kPolygonCount = 32u;
    runtime.asset.polygons = makeLinearNavPolygons(kPolygonCount);
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{glm::vec3(0.25f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    profiler.startCapture();
    profiler.beginFrame();
    assert(navigation.setAgentDestination(
        world,
        runtime,
        agentEntity,
        glm::vec3(static_cast<float>(kPolygonCount) - 0.25f, 0.0f, 0.0f)
    ));
    profiler.endFrame({});
    profiler.waitForWorkerIdle();

    const core::ProfilerTraceCapture capture = profiler.rawCapture();
    assert(capture.frames.size() == 1u);
    assert(containsRecordedScopeNamed(capture.frames[0].cpuScopes, "Navigation Pathfind"));
    const std::optional<std::uint64_t> durationNs = findRecordedScopeDurationNs(
        capture.frames[0].cpuScopes,
        "Navigation Pathfind"
    );
    assert(durationNs.has_value());
    assert(*durationNs >= 5'000u);
}

void testNavigationAsyncRequestKeepsCurrentPathUntilReady() {
    core::TaskScheduler scheduler = makeScheduler();
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = makeLinearNavPolygons(24u);
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{glm::vec3(0.25f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    const glm::vec3 initialDestination(5.75f, 0.0f, 0.0f);
    const glm::vec3 requestedDestination(23.75f, 0.0f, 0.0f);
    assert(navigation.setAgentDestination(world, runtime, agentEntity, initialDestination));
    const std::vector<glm::vec3> originalPath = world.navAgents.get(agentEntity).pathCorners;
    assert(!originalPath.empty());

    assert(navigation.requestAgentDestination(world, runtime, scheduler, agentEntity, requestedDestination));
    const auto countersWhilePending = navigation.profilingCounters();
    assert(requireCounterValue(countersWhilePending, "Pending Path Requests", "Navigation") == 1);
    assert(requireCounterValue(countersWhilePending, "Failed Path Requests", "Navigation") == 0);
    assert(requireCounterValue(countersWhilePending, "Stale Path Results", "Navigation") == 0);

    const core::NavAgentComponent& pendingAgent = world.navAgents.get(agentEntity);
    assert(pendingAgent.destination.has_value());
    assert(nearlyEqualVec3(*pendingAgent.destination, initialDestination));
    assert(pathsEqual(pendingAgent.pathCorners, originalPath));

    waitForNavigationRequestsToDrain(navigation, world, runtime);

    const core::NavAgentComponent& updatedAgent = world.navAgents.get(agentEntity);
    assert(updatedAgent.destination.has_value());
    assert(nearlyEqualVec3(*updatedAgent.destination, requestedDestination));
    assert(!updatedAgent.pathCorners.empty());
    assert(nearlyEqualVec3(updatedAgent.pathCorners.back(), requestedDestination));

    const auto countersAfterApply = navigation.profilingCounters();
    assert(requireCounterValue(countersAfterApply, "Pending Path Requests", "Navigation") == 0);
    assert(requireCounterValue(countersAfterApply, "Last Async Pathfind Us", "Navigation") > 0);
}

void testNavigationAsyncLatestClickWinsAndCountsStaleResults() {
    core::TaskScheduler scheduler = makeScheduler();
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = makeLinearNavPolygons(24u);
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{glm::vec3(0.25f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    const glm::vec3 firstDestination(11.75f, 0.0f, 0.0f);
    const glm::vec3 secondDestination(23.75f, 0.0f, 0.0f);
    assert(navigation.requestAgentDestination(world, runtime, scheduler, agentEntity, firstDestination));
    assert(navigation.requestAgentDestination(world, runtime, scheduler, agentEntity, secondDestination));

    const auto countersWhilePending = navigation.profilingCounters();
    assert(requireCounterValue(countersWhilePending, "Pending Path Requests", "Navigation") == 1);
    assert(requireCounterValue(countersWhilePending, "Stale Path Results", "Navigation") == 1);

    waitForNavigationRequestsToDrain(navigation, world, runtime);

    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(agent.destination.has_value());
    assert(nearlyEqualVec3(*agent.destination, secondDestination));
    assert(!agent.pathCorners.empty());
    assert(nearlyEqualVec3(agent.pathCorners.back(), secondDestination));

    const auto countersAfterApply = navigation.profilingCounters();
    assert(requireCounterValue(countersAfterApply, "Pending Path Requests", "Navigation") == 0);
    assert(requireCounterValue(countersAfterApply, "Stale Path Results", "Navigation") == 1);
}

void testNavigationAsyncRebuildInvalidatesPendingRequests() {
    core::TaskScheduler scheduler = makeScheduler();
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = makeLinearNavPolygons(24u);
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{glm::vec3(0.25f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    const glm::vec3 originalDestination(5.75f, 0.0f, 0.0f);
    assert(navigation.setAgentDestination(world, runtime, agentEntity, originalDestination));
    const std::vector<glm::vec3> originalPath = world.navAgents.get(agentEntity).pathCorners;

    assert(navigation.requestAgentDestination(world, runtime, scheduler, agentEntity, glm::vec3(23.75f, 0.0f, 0.0f)));
    assert(requireCounterValue(navigation.profilingCounters(), "Pending Path Requests", "Navigation") == 1);

    assert(navigation.rebuildRuntime(runtime));
    navigation.applyCompletedPathRequests(world, runtime);

    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(agent.destination.has_value());
    assert(nearlyEqualVec3(*agent.destination, originalDestination));
    assert(pathsEqual(agent.pathCorners, originalPath));

    const auto countersAfterRebuild = navigation.profilingCounters();
    assert(requireCounterValue(countersAfterRebuild, "Pending Path Requests", "Navigation") == 0);
    assert(requireCounterValue(countersAfterRebuild, "Stale Path Results", "Navigation") == 1);
}

void testNavigationAsyncFailurePreservesCurrentPathAndCountsFailures() {
    core::TaskScheduler scheduler = makeScheduler();
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(4.0f, -1.0f), glm::vec2(5.0f, -1.0f), glm::vec2(5.0f, 1.0f), glm::vec2(4.0f, 1.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{glm::vec3(0.25f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    const glm::vec3 originalDestination(0.75f, 0.0f, 0.0f);
    assert(navigation.setAgentDestination(world, runtime, agentEntity, originalDestination));
    const std::vector<glm::vec3> originalPath = world.navAgents.get(agentEntity).pathCorners;

    assert(navigation.requestAgentDestination(world, runtime, scheduler, agentEntity, glm::vec3(4.75f, 0.0f, 0.0f)));
    waitForNavigationRequestsToDrain(navigation, world, runtime);

    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(agent.destination.has_value());
    assert(nearlyEqualVec3(*agent.destination, originalDestination));
    assert(pathsEqual(agent.pathCorners, originalPath));

    const auto counters = navigation.profilingCounters();
    assert(requireCounterValue(counters, "Pending Path Requests", "Navigation") == 0);
    assert(requireCounterValue(counters, "Failed Path Requests", "Navigation") == 1);
    assert(requireCounterValue(counters, "Stale Path Results", "Navigation") == 0);
}

void testNavigationAsyncProfilerCaptureIncludesClickScopesAndCounters() {
    core::ProfilerConfig config{};
    config.maxFrames = 4u;
    config.maxCpuScopesPerFrame = 128u;
    core::ProfilerService profiler(config);

    core::TaskScheduler scheduler = makeScheduler();
    scheduler.setProfiler(&profiler);

    core::NavigationSystem navigation;
    navigation.setProfiler(&profiler);

    core::NavigationRuntime runtime{};
    runtime.asset.polygons = makeLinearNavPolygons(24u, -4.0f);
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{glm::vec3(-3.75f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    const render::CameraMatrices camera = core::computeCameraMatrices(core::CameraState{}, 100, 100);
    const glm::vec3 requestedDestination(19.75f, 0.0f, 0.0f);

    profiler.startCapture();
    profiler.beginFrame();
    {
        auto eventDispatch = profiler.scopedCpu("Event Dispatch");
        (void)eventDispatch;
        auto viewportClick = profiler.scopedCpu("Viewport Click");
        (void)viewportClick;
        auto gameplayClickMove = profiler.scopedCpu("Gameplay Click Move");
        (void)gameplayClickMove;

        std::optional<core::NavHitResult> hit{};
        {
            auto hitScope = profiler.scopedCpu("Navigation Hit Test");
            (void)hitScope;
            hit = navigation.hitTest(runtime, camera, 100, 100, 50, 50);
        }
        assert(hit.has_value());

        {
            auto requestScope = profiler.scopedCpu("Navigation Path Request");
            (void)requestScope;
            assert(navigation.requestAgentDestination(world, runtime, scheduler, agentEntity, requestedDestination));
        }
    }

    for (int attempt = 0; attempt < 200; ++attempt) {
        {
            auto applyScope = profiler.scopedCpu("Navigation Path Apply");
            (void)applyScope;
            navigation.applyCompletedPathRequests(world, runtime);
        }
        if (requireCounterValue(navigation.profilingCounters(), "Pending Path Requests", "Navigation") == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto counters = navigation.profilingCounters();
    assert(requireCounterValue(counters, "Pending Path Requests", "Navigation") == 0);
    assert(requireCounterValue(counters, "Last Async Pathfind Us", "Navigation") > 0);
    profiler.endFrame({}, counters);
    profiler.waitForWorkerIdle();

    const core::ProfilerTraceCapture capture = profiler.rawCapture();
    assert(capture.frames.size() == 1u);
    assert(containsRecordedScopeNamed(capture.frames[0].cpuScopes, "Event Dispatch"));
    assert(containsRecordedScopeNamed(capture.frames[0].cpuScopes, "Viewport Click"));
    assert(containsRecordedScopeNamed(capture.frames[0].cpuScopes, "Gameplay Click Move"));
    assert(containsRecordedScopeNamed(capture.frames[0].cpuScopes, "Navigation Hit Test"));
    assert(containsRecordedScopeNamed(capture.frames[0].cpuScopes, "Navigation Path Request"));
    assert(containsRecordedScopeNamed(capture.frames[0].cpuScopes, "Navigation Path Apply"));
    assert(requireCounterValue(capture.frames[0].counters, "Pending Path Requests", "Navigation") == 0);
    assert(requireCounterValue(capture.frames[0].counters, "Last Async Pathfind Us", "Navigation") > 0);
    assert(requireCounterValue(capture.frames[0].counters, "Failed Path Requests", "Navigation") == 0);
    assert(requireCounterValue(capture.frames[0].counters, "Stale Path Results", "Navigation") == 0);
}

void testRuntimePolicyEnablesHeavyWorkloadsAndUsesHysteresis() {
    core::RuntimePolicy policy;
    core::RuntimePolicyFrameContext context{};
    context.workerCount = 4u;
    context.transformsDirty = true;
    context.lightsDirty = true;
    context.transformCount = 512u;
    context.boundsCount = 256u;
    context.renderableCount = 640u;
    context.parentCount = 256u;
    context.pointLightCount = 48u;
    context.spotLightCount = 16u;
    context.lightVolumeCount = 8u;

    const core::RuntimePolicyDecision& heavyDecision = policy.evaluate(context);
    assert(heavyDecision.parallelTransformUpdate);
    assert(heavyDecision.parallelLightUpdate);
    assert(heavyDecision.parallelRenderExtraction);
    assert(heavyDecision.parallelSceneView);

    context.transformCount = 96u;
    context.boundsCount = 64u;
    context.renderableCount = 320u;
    context.parentCount = 64u;
    context.pointLightCount = 16u;
    context.spotLightCount = 8u;
    context.lightVolumeCount = 8u;

    const core::RuntimePolicyDecision& mediumDecision = policy.evaluate(context);
    assert(mediumDecision.parallelTransformUpdate);
    assert(mediumDecision.parallelLightUpdate);
    assert(mediumDecision.parallelRenderExtraction);
    assert(mediumDecision.parallelSceneView);

    context.transformCount = 32u;
    context.boundsCount = 16u;
    context.renderableCount = 96u;
    context.parentCount = 16u;
    context.pointLightCount = 4u;
    context.spotLightCount = 0u;
    context.lightVolumeCount = 2u;

    const core::RuntimePolicyDecision& lowDecision = policy.evaluate(context);
    assert(!lowDecision.parallelTransformUpdate);
    assert(!lowDecision.parallelLightUpdate);
    assert(!lowDecision.parallelRenderExtraction);
    assert(!lowDecision.parallelSceneView);

    const auto counters = policy.profilingCounters();
    assert(findCounterValue(counters, "Transform Update Enabled", "Runtime Policy").value_or(-1) == 0);
    assert(findCounterValue(counters, "Scene View Enabled", "Runtime Policy").value_or(-1) == 0);
    assert(findCounterValue(counters, "Worker Threads", "Runtime Policy").value_or(-1) == 4);
}

void testRuntimePolicyDisablesParallelismWithoutEnoughWorkers() {
    core::RuntimePolicy policy;
    const core::RuntimePolicyDecision& decision = policy.evaluate(core::RuntimePolicyFrameContext{
        1u,
        true,
        true,
        2048u,
        1024u,
        2048u,
        1024u,
        256u,
        128u,
        32u,
    });

    assert(!decision.parallelTransformUpdate);
    assert(!decision.parallelLightUpdate);
    assert(!decision.parallelRenderExtraction);
    assert(!decision.parallelSceneView);
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
        }, {
            render::FrameCounterRecord{
                "Culled",
                frame == 2 ? 7 : 3,
                "Frustum Culling"
            },
            render::FrameCounterRecord{
                "Occluded",
                frame == 2 ? 4 : 1,
                "Occlusion Culling"
            }
        });
    }
    profiler.waitForWorkerIdle();

    const auto snapshots = profiler.snapshots();
    assert(snapshots.size() == 2u);
    assert(snapshots.back()->counters.size() == 2u);
    assert(findCounterValue(snapshots.back()->counters, "Culled", "Frustum Culling").value_or(-1) == 7);
    assert(findCounterValue(snapshots.back()->counters, "Occluded", "Occlusion Culling").value_or(-1) == 4);

    const core::ProfilerTraceCapture capture = profiler.rawCapture();
    assert(capture.mainThreadId == profiler.mainThreadId());
    assert(capture.frames.size() == 2u);
    assert(capture.frames.front().frameNumber == 2u);
    assert(capture.frames.back().frameNumber == 3u);
    assert(capture.frames.back().cpuScopes.size() == 1u);
    assert(capture.frames.back().cpuScopes[0].name == "Third");
    assert(capture.frames.back().resources.size() == 1u);
    assert(capture.frames.back().resources[0].gpuBytes == 3072u);
    assert(capture.frames.back().counters.size() == 2u);
    assert(findCounterValue(capture.frames.back().counters, "Culled", "Frustum Culling").value_or(-1) == 7);
    assert(findCounterValue(capture.frames.back().counters, "Occluded", "Occlusion Culling").value_or(-1) == 4);
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
            },
            {
                render::FrameCounterRecord{"Culled", 3, "Frustum Culling"},
                render::FrameCounterRecord{"Occluded", 2, "Occlusion Culling"},
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
            },
            {
                render::FrameCounterRecord{"Culled", 1, "Frustum Culling"},
                render::FrameCounterRecord{"Occluded", 0, "Occlusion Culling"},
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
    assert(bytes.find("GPU Timed Passes") != std::string::npos);
    assert(bytes.find("Metrics") != std::string::npos);
    assert(bytes.find("Frustum Culling") != std::string::npos);
    assert(bytes.find("Occlusion Culling") != std::string::npos);
    assert(bytes.find("Culled") != std::string::npos);
    assert(bytes.find("Occluded") != std::string::npos);
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

    assert(descriptorCount >= 12u);
    assert(sliceBeginCount == 6u);
    assert(sliceEndCount == 6u);
    assert(counterCount == 12u);

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
    testNavigationAssetRoundTrip();
    testNavigationBakeBuildsMultiLevelPolygonsAndBlocksOnlyOverlappingLayers();
    testNavigationPathfindingUsesSameLayerFunnelAndExplicitLinks();
    testNavigationOverlapCorridorConstraint();
    testNavigationConcavePolygonStaysInsideWalkableSurface();
    testNavigationOverlapPrefersShortestStraightCorridor();
    testNavigationOverlapCandidateSelectionUsesMultiplyCoveredStartCell();
    testNavigationRejectsSelfIntersectingPolygon();
    testNavigationHitTestFindsProjectedNavPolygon();
    testNavigationAgentMovementRotatesAndRequestsWalkThenIdle();
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
    testJointInfluenceBoundsBuildsSingleJointSection();
    testJointInfluenceBoundsBuildsMixedWeightSection();
    testRenderExtractionAlignsSkinnedChildBoundsWithRenderedMesh();
    testRenderExtractionConservativeSkinnedBoundsContainExactBounds();
    testRenderExtractionFiltersHelperSkeletonBranches();
    testRenderSceneViewBuildResolvesRenderableSelection();
    testRenderSceneViewBuildResolvesLightSelection();
    testRenderSceneViewBuildResolvesNodeSelection();
    testCameraFrustumCullingUpdatesVisibilityAndStats();
    testCameraFrustumCullingRejectsFarPlaneBounds();
    testOcclusionCullingUsesLastKnownResultsAndWarmup();
    testOcclusionCullingHidesAfterConsecutiveOccludedResults();
    testOcclusionCullingTreatsFrustumRejectedObjectsAsNonCandidates();
    testFramePreparationRemainsStableWithSingleWorker();
    testFramePreparationRemainsStableWithMultipleWorkers();
    testActiveLightSelectionDeduplicatesAcrossVolumes();
    testPickingSystemCanIgnoreLights();
    testOverlayWorkSkipsAllOverlayWorkWhenDisabled();
    testOverlayWorkCullsIconsAndBatchesSelectedLights();
    testOverlayWorkKeepsSelectedLightDebugWhenLightDebugIsOff();
    testProfilerScopeTreeAggregatesNestedScopes();
    testProfilerServiceStartStopAndFrameRingBuffer();
    testProfilerServiceDropsExcessCpuScopes();
    testProfilerServiceCapturesWorkerThreadScopes();
    testNavigationPathfindingEmitsProfilerScope();
    testNavigationAsyncRequestKeepsCurrentPathUntilReady();
    testNavigationAsyncLatestClickWinsAndCountsStaleResults();
    testNavigationAsyncRebuildInvalidatesPendingRequests();
    testNavigationAsyncFailurePreservesCurrentPathAndCountsFailures();
    testNavigationAsyncProfilerCaptureIncludesClickScopesAndCounters();
    testRuntimePolicyEnablesHeavyWorkloadsAndUsesHysteresis();
    testRuntimePolicyDisablesParallelismWithoutEnoughWorkers();
    testProfilerServiceRetainsRawFramesForExport();
    testPerfettoTraceExporterWritesTrackEventsAndCounters();
    testPerfettoTraceExporterFailsForInvalidOutputPath();
    testProfilingMemoryEstimators();
    return EXIT_SUCCESS;
}
