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
#include <numeric>
#include <optional>
#include <random>
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
#include "core/navigation/Navigation.hpp"
#include "core/physics/PhysicsSystem.hpp"
#include "core/scene/Camera.hpp"
#include "core/systems/PickingSystem.hpp"
#include "core/profiling/ProfilerService.hpp"
#include "core/systems/RenderExtractionSystem.hpp"
#include "core/editor/SelectionModel.hpp"
#include "core/systems/TaskScheduler.hpp"
#include "core/transform/TransformMath.hpp"
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

std::shared_ptr<render::Material> makeMaterial(std::string_view name = {}) {
    auto material = std::make_shared<render::Material>();
    material->name = std::string(name);
    return material;
}

core::EntityId addLightVolumeEntity(core::World& world, const glm::vec3& position, const glm::vec3& halfExtents) {
    const core::EntityId entity = world.createEntity();
    world.transforms.emplace(entity, core::TransformComponent{position, glm::vec3(0.0f), glm::vec3(1.0f)});
    world.lightVolumes.emplace(entity, core::LightVolumeComponent{halfExtents});
    world.markTransformsDirty(entity);
    return entity;
}

core::FrameRenderable makeFrameRenderable(
    core::EntityId entity,
    render::MeshHandle mesh,
    render::RenderLayer layer,
    const render::Bounds3& localBounds,
    const render::Bounds3& worldBounds,
    bool hasWorldBounds = true,
    bool visible = true
) {
    core::FrameRenderable renderable{};
    renderable.entity = entity;
    renderable.mesh = mesh;
    renderable.layer = layer;
    renderable.localBounds = localBounds;
    renderable.worldBounds = worldBounds;
    renderable.hasWorldBounds = hasWorldBounds;
    renderable.modelMatrix = glm::mat4(1.0f);
    renderable.visible = visible;
    return renderable;
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

render::Mesh makeLShapedFootprintMesh() {
    render::Mesh mesh{};
    mesh.positions = {
        glm::vec3(-2.0f, 0.0f, -2.0f),
        glm::vec3(0.0f, 0.0f, -2.0f),
        glm::vec3(0.0f, 0.0f, 2.0f),
        glm::vec3(-2.0f, 0.0f, 2.0f),
        glm::vec3(2.0f, 0.0f, -2.0f),
        glm::vec3(2.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
    };
    mesh.indices = {
        0u, 1u, 2u,
        0u, 2u, 3u,
        1u, 4u, 5u,
        1u, 5u, 6u,
    };
    return mesh;
}

glm::vec2 polygonCenterXZ(const core::NavPolygon& polygon) {
    glm::vec2 center(0.0f);
    for (const glm::vec2& vertex : polygon.verticesXZ) {
        center += vertex;
    }
    return center / static_cast<float>(polygon.verticesXZ.size());
}

bool pointInsideWalkableUnion(
    const glm::vec3& point,
    const std::vector<core::NavPolygon>& polygons
);

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

bool envFlagEnabled(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return false;
    }
    const std::string_view token(value);
    return token != "0" &&
        token != "false" &&
        token != "FALSE" &&
        token != "off" &&
        token != "OFF" &&
        token != "no" &&
        token != "NO";
}

std::size_t envSizeTOr(const char* name, std::size_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || (end != nullptr && *end != '\0')) {
        return fallback;
    }
    return static_cast<std::size_t>(parsed);
}

double envDoubleOr(const char* name, double fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || (end != nullptr && *end != '\0')) {
        return fallback;
    }
    return parsed;
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
    session.navMeshPolygonWireframeVisible = true;
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
    assert(ini.find("NavMeshPolygonWireframeVisible=1") != std::string::npos);
    assert(ini.find("ProfilerFollowLatest=0") != std::string::npos);
    ImGui::DestroyContext();

    core::EditorSession restored;
    restored.sceneHierarchyVisible = true;
    restored.inspectorWindowVisible = true;
    restored.profilerWindowVisible = false;
    restored.navMeshWindowVisible = true;
    restored.navMeshOverlayVisible = true;
    restored.navMeshPolygonWireframeVisible = false;
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
    assert(restored.navMeshPolygonWireframeVisible);
    assert(!restored.profilerFollowLatest);
    ImGui::DestroyContext();
}

void testSelectionModelTracksComponentFocus() {
    core::SelectionModel selection;
    const core::EntityId entity{42, 1};

    selection.set(std::optional<core::SelectionTarget>{core::SelectionTarget{entity, core::ComponentKind::Material}});
    assert(selection.current().has_value());
    assert(selection.current()->entity == entity);
    assert(selection.current()->component.has_value());
    assert(*selection.current()->component == core::ComponentKind::Material);

    selection.set(entity);
    assert(selection.current().has_value());
    assert(selection.current()->entity == entity);
    assert(!selection.current()->component.has_value());

    selection.clear();
    assert(!selection.current().has_value());
}

void testNavigationAssetRoundTrip() {
    core::NavMeshAsset asset{};
    asset.minimumRuntimeCellArea = 0.0125f;
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
    assert(nearlyEqual(parsed.minimumRuntimeCellArea, 0.0125, 0.000001));
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

void testNavigationBakeFiltersRuntimeCellsBelowConfiguredArea() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.minimumRuntimeCellArea = 0.01f;
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {
            glm::vec2(0.0f, 0.0f),
            glm::vec2(2.0f, 0.0f),
            glm::vec2(2.0f, 2.0f),
            glm::vec2(0.0f, 2.0f)
        }},
        core::NavPolygon{2, 0.0f, {
            glm::vec2(2.0f, 0.975f),
            glm::vec2(2.05f, 0.975f),
            glm::vec2(2.05f, 1.025f),
            glm::vec2(2.0f, 1.025f)
        }},
        core::NavPolygon{3, 0.0f, {
            glm::vec2(2.05f, 0.0f),
            glm::vec2(4.05f, 0.0f),
            glm::vec2(4.05f, 2.0f),
            glm::vec2(2.05f, 2.0f)
        }},
    };

    assert(navigation.rebuildRuntime(runtime));
    assert(runtime.bakedCells.size() == 2u);
    assert(runtime.filteredRuntimeCellCount == 1u);
    assert(runtime.polygonToCellIndices.size() == 3u);
    assert(runtime.polygonToCellIndices[0].size() == 1u);
    assert(runtime.polygonToCellIndices[1].empty());
    assert(runtime.polygonToCellIndices[2].size() == 1u);

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{
        glm::vec3(1.0f, 0.0f, 1.0f),
        glm::vec3(0.0f),
        glm::vec3(1.0f)
    });
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});
    assert(!navigation.setAgentDestination(world, runtime, agentEntity, glm::vec3(3.0f, 0.0f, 1.0f)));
}

void testNavigationGenerationUsesUnionOfObjectColliderShapes() {
    core::World world;
    core::TransformSystem transformSystem;
    core::TaskScheduler scheduler = makeScheduler();

    const core::EntityId ground = world.createEntity();
    world.transforms.emplace(ground, core::TransformComponent{});
    world.navSources.emplace(ground, core::NavSourceComponent{"Ground", core::NavSourceTag::Walkable, core::NavSourceTag::Walkable});
    world.navSourceGeometry.emplace(ground, core::NavSourceGeometryComponent{std::make_shared<render::Mesh>(makeHorizontalQuadMesh(-5.0f, 5.0f, -5.0f, 5.0f, 0.0f))});

    const core::EntityId object = world.createEntity();
    world.transforms.emplace(object, core::TransformComponent{});

    const core::EntityId visual = world.createEntity();
    world.parents.emplace(visual, core::ParentComponent{object});
    world.transforms.emplace(visual, core::TransformComponent{});
    world.navSources.emplace(visual, core::NavSourceComponent{"Object/Visual", core::NavSourceTag::Blocking, core::NavSourceTag::Blocking});
    world.navSourceGeometry.emplace(visual, core::NavSourceGeometryComponent{std::make_shared<render::Mesh>(makeBoxVertexMesh(glm::vec3(-4.0f, 0.0f, -4.0f), glm::vec3(4.0f, 2.0f, 4.0f)))});

    const core::EntityId boxShape = world.createEntity();
    world.parents.emplace(boxShape, core::ParentComponent{object});
    world.transforms.emplace(boxShape, core::TransformComponent{glm::vec3(-2.0f, 0.5f, 0.0f), glm::vec3(0.0f, 45.0f, 0.0f), glm::vec3(1.0f)});
    world.boxColliders.emplace(boxShape, core::BoxColliderComponent{glm::vec3(0.0f), glm::vec3(1.0f, 0.5f, 0.25f), false, false});

    const core::EntityId sphereShape = world.createEntity();
    world.parents.emplace(sphereShape, core::ParentComponent{object});
    world.transforms.emplace(sphereShape, core::TransformComponent{glm::vec3(2.0f, 0.75f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.sphereColliders.emplace(sphereShape, core::SphereColliderComponent{glm::vec3(0.0f), 0.75f, false, false});

    world.markTransformsDirty(object);
    transformSystem.update(world, scheduler, false);

    core::NavigationRuntime runtime{};
    core::NavigationSystem navigation;
    std::string error{};
    assert(navigation.generateFromTags(world, runtime, &error));
    assert(error.empty());

    assert(!pointInsideWalkableUnion(glm::vec3(-2.0f, 0.0f, 0.0f), runtime.asset.polygons));
    assert(!pointInsideWalkableUnion(glm::vec3(2.55f, 0.0f, 0.25f), runtime.asset.polygons));
    assert(pointInsideWalkableUnion(glm::vec3(0.0f, 0.0f, 0.0f), runtime.asset.polygons));
    assert(pointInsideWalkableUnion(glm::vec3(-1.25f, 0.0f, 0.75f), runtime.asset.polygons));
    assert(pointInsideWalkableUnion(glm::vec3(3.0f, 0.0f, 0.0f), runtime.asset.polygons));
}

void testNavigationDefaultHitboxPreservesConcaveShapeUnion() {
    core::World world;
    core::TransformSystem transformSystem;
    core::TaskScheduler scheduler = makeScheduler();

    const core::EntityId ground = world.createEntity();
    world.transforms.emplace(ground, core::TransformComponent{});
    world.navSources.emplace(ground, core::NavSourceComponent{"Ground", core::NavSourceTag::Walkable, core::NavSourceTag::Walkable});
    world.navSourceGeometry.emplace(ground, core::NavSourceGeometryComponent{std::make_shared<render::Mesh>(makeHorizontalQuadMesh(-4.0f, 4.0f, -4.0f, 4.0f, 0.0f))});

    const core::EntityId blocker = world.createEntity();
    world.transforms.emplace(blocker, core::TransformComponent{});
    world.navSources.emplace(blocker, core::NavSourceComponent{"L Blocker", core::NavSourceTag::Blocking, core::NavSourceTag::Blocking});
    world.navSourceGeometry.emplace(blocker, core::NavSourceGeometryComponent{std::make_shared<render::Mesh>(makeLShapedFootprintMesh())});
    world.markTransformsDirty(blocker);
    transformSystem.update(world, scheduler, false);

    core::NavigationRuntime runtime{};
    core::NavigationSystem navigation;
    std::string error{};
    assert(navigation.generateFromTags(world, runtime, &error));
    assert(error.empty());

    assert(!pointInsideWalkableUnion(glm::vec3(-1.0f, 0.0f, 1.0f), runtime.asset.polygons));
    assert(!pointInsideWalkableUnion(glm::vec3(1.0f, 0.0f, -1.0f), runtime.asset.polygons));
    assert(pointInsideWalkableUnion(glm::vec3(1.0f, 0.0f, 1.0f), runtime.asset.polygons));
}

void testNavigationGenerationMinimizesRectangularObstacleDecomposition() {
    core::World world;
    core::TransformSystem transformSystem;
    core::TaskScheduler scheduler = makeScheduler();

    const core::EntityId ground = world.createEntity();
    world.transforms.emplace(ground, core::TransformComponent{});
    world.navSources.emplace(ground, core::NavSourceComponent{"Ground", core::NavSourceTag::Walkable, core::NavSourceTag::Walkable});
    world.navSourceGeometry.emplace(ground, core::NavSourceGeometryComponent{std::make_shared<render::Mesh>(makeHorizontalQuadMesh(-5.0f, 5.0f, -5.0f, 5.0f, 0.0f))});

    const core::EntityId blocker = world.createEntity();
    world.transforms.emplace(blocker, core::TransformComponent{glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navSources.emplace(blocker, core::NavSourceComponent{"Blocker", core::NavSourceTag::Blocking, core::NavSourceTag::Blocking});
    world.navSourceGeometry.emplace(blocker, core::NavSourceGeometryComponent{std::make_shared<render::Mesh>(makeBoxVertexMesh(glm::vec3(-4.0f), glm::vec3(4.0f)))});
    world.boxColliders.emplace(blocker, core::BoxColliderComponent{glm::vec3(0.0f), glm::vec3(1.0f, 0.5f, 1.0f), false, false});
    world.markTransformsDirty(blocker);
    transformSystem.update(world, scheduler, false);

    core::NavigationRuntime runtime{};
    core::NavigationSystem navigation;
    std::string error{};
    assert(navigation.generateFromTags(world, runtime, &error));
    assert(error.empty());
    assert(runtime.asset.polygons.size() == 4u);
}

void testNavigationGenerationHandlesFantasyHouseGeometryWithoutPathologicalMerge() {
    render::GltfModelData model{};
    assert(render::loadGltfModel(assetPath("src/render/models/FantasyHouse.glb").string(), model));

    core::World world;
    core::TransformSystem transformSystem;
    core::TaskScheduler scheduler = makeScheduler();

    const core::EntityId ground = world.createEntity();
    world.transforms.emplace(ground, core::TransformComponent{});
    world.navSources.emplace(ground, core::NavSourceComponent{"Ground", core::NavSourceTag::Walkable, core::NavSourceTag::Walkable});
    world.navSourceGeometry.emplace(ground, core::NavSourceGeometryComponent{std::make_shared<render::Mesh>(makeHorizontalQuadMesh(-20.0f, 20.0f, -20.0f, 20.0f, 0.0f))});

    const core::EntityId house = world.createEntity();
    world.transforms.emplace(house, core::TransformComponent{});
    for (std::size_t sectionIndex = 0; sectionIndex < model.sections.size(); ++sectionIndex) {
        const render::GltfMeshSection& section = model.sections[sectionIndex];
        const core::EntityId sectionEntity = world.createEntity();
        world.parents.emplace(sectionEntity, core::ParentComponent{house});

        core::TransformComponent transform{};
        if (section.nodeIndex >= 0 && section.nodeIndex < static_cast<int>(model.nodes.size())) {
            render::NodeTransform nodeTransform{};
            if (render::decomposeNodeTransform(
                    model.nodes[static_cast<std::size_t>(section.nodeIndex)].bindGlobalMatrix,
                    nodeTransform)) {
                transform.position = nodeTransform.translation;
                transform.rotationDeg = glm::degrees(glm::eulerAngles(nodeTransform.rotation));
                transform.scale = nodeTransform.scale;
            }
        }
        world.transforms.emplace(sectionEntity, transform);
        world.navSources.emplace(sectionEntity, core::NavSourceComponent{
            "House/" + std::to_string(sectionIndex),
            core::NavSourceTag::Blocking,
            core::NavSourceTag::Blocking
        });
        world.navSourceGeometry.emplace(sectionEntity, core::NavSourceGeometryComponent{
            std::make_shared<render::Mesh>(section.mesh)
        });
    }

    world.markTransformsDirty(house);
    transformSystem.update(world, scheduler, false);

    core::NavigationRuntime runtime{};
    core::NavigationSystem navigation;
    std::size_t expectedPolygonCount = 0u;
    double slowestDurationSeconds = 0.0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string error{};
        const auto startedAt = std::chrono::steady_clock::now();
        assert(navigation.generateFromTags(world, runtime, &error));
        const double durationSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::steady_clock::now() - startedAt).count();
        slowestDurationSeconds = std::max(slowestDurationSeconds, durationSeconds);

        assert(error.empty());
        assert(!runtime.asset.polygons.empty());
        if (attempt == 0) {
            expectedPolygonCount = runtime.asset.polygons.size();
        } else {
            assert(runtime.asset.polygons.size() == expectedPolygonCount);
        }
    }
    std::printf(
        "[house navgen] polygons=%zu duration_ms=%.3f\n",
        runtime.asset.polygons.size(),
        slowestDurationSeconds * 1000.0
    );
    assert(slowestDurationSeconds < 2.0);

    std::string bakeError{};
    const auto bakeStartedAt = std::chrono::steady_clock::now();
    assert(navigation.rebuildRuntime(runtime, &bakeError));
    const double bakeDurationSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::steady_clock::now() - bakeStartedAt).count();
    assert(bakeError.empty());
    assert(!runtime.bakedCells.empty());
    const std::size_t bakedVertexCount = std::accumulate(
        runtime.bakedCells.begin(),
        runtime.bakedCells.end(),
        std::size_t{0u},
        [](std::size_t count, const core::NavRuntimeCell& cell) { return count + cell.verticesXZ.size(); }
    );
    const std::size_t graphEdgeCount = std::accumulate(
        runtime.graph.begin(),
        runtime.graph.end(),
        std::size_t{0u},
        [](std::size_t count, const std::vector<core::NavGraphEdge>& edges) { return count + edges.size(); }
    );
    std::printf(
        "[house bake] cells=%zu vertices=%zu graph_edges=%zu duration_ms=%.3f\n",
        runtime.bakedCells.size(),
        bakedVertexCount,
        graphEdgeCount,
        bakeDurationSeconds * 1000.0
    );
    assert(bakeDurationSeconds < 2.0);

    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});
    const std::array<std::pair<glm::vec3, glm::vec3>, 4u> pathCases{{
        {glm::vec3(-15.0f, 0.0f, 0.0f), glm::vec3(15.0f, 0.0f, 0.0f)},
        {glm::vec3(0.0f, 0.0f, -15.0f), glm::vec3(0.0f, 0.0f, 15.0f)},
        {glm::vec3(-15.0f, 0.0f, -15.0f), glm::vec3(15.0f, 0.0f, 15.0f)},
        {glm::vec3(-15.0f, 0.0f, 15.0f), glm::vec3(15.0f, 0.0f, -15.0f)},
    }};
    double slowestPathSeconds = 0.0;
    for (const auto& [start, destination] : pathCases) {
        world.transforms.get(agentEntity).position = start;
        const auto pathStartedAt = std::chrono::steady_clock::now();
        assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
        slowestPathSeconds = std::max(
            slowestPathSeconds,
            std::chrono::duration_cast<std::chrono::duration<double>>(
                std::chrono::steady_clock::now() - pathStartedAt
            ).count()
        );
    }
    std::printf("[house pathfind] slowest_ms=%.3f\n", slowestPathSeconds * 1000.0);
    assert(slowestPathSeconds < 1.0 / 60.0);

    core::NavAgentComponent& clearanceAgent = world.navAgents.get(agentEntity);
    clearanceAgent.clearanceSource = core::NavAgentClearanceSource::BoxCollider;
    world.boxColliders.emplace(agentEntity, core::BoxColliderComponent{
        glm::vec3(0.0f),
        glm::vec3(0.22f, 0.5f, 0.22f),
        false,
        false
    });
    double slowestClearancePathSeconds = 0.0;
    for (const auto& [start, destination] : pathCases) {
        world.transforms.get(agentEntity).position = start;
        const auto pathStartedAt = std::chrono::steady_clock::now();
        assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
        slowestClearancePathSeconds = std::max(
            slowestClearancePathSeconds,
            std::chrono::duration_cast<std::chrono::duration<double>>(
                std::chrono::steady_clock::now() - pathStartedAt
            ).count()
        );
    }
    std::printf("[house clearance pathfind] slowest_ms=%.3f\n", slowestClearancePathSeconds * 1000.0);
    assert(slowestClearancePathSeconds < 1.0 / 60.0);
}

void testNavigationPathfindingUsesIntervalSearchAndExplicitLinks() {
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

bool boxFootprintFitsInsidePolygon(
    const glm::vec2& center,
    const core::NavPolygon& polygon,
    const glm::vec2& halfExtentsXZ
) {
    const float radius = glm::length(halfExtentsXZ);
    constexpr int kDirections = 16;
    if (!pointInOrOnPolygonXZ(center, polygon.verticesXZ)) {
        return false;
    }
    for (int directionIndex = 0; directionIndex < kDirections; ++directionIndex) {
        const float angle = static_cast<float>(directionIndex) / static_cast<float>(kDirections) * 6.28318530717958647692f;
        const glm::vec2 offset(std::cos(angle) * radius, std::sin(angle) * radius);
        if (!pointInOrOnPolygonXZ(center + offset, polygon.verticesXZ)) {
            return false;
        }
    }
    return true;
}

std::optional<glm::vec3> sampleRandomPointInsidePolygonForBox(
    const core::NavPolygon& polygon,
    const glm::vec2& halfExtentsXZ,
    std::mt19937& rng
) {
    glm::vec2 minPoint(std::numeric_limits<float>::max());
    glm::vec2 maxPoint(-std::numeric_limits<float>::max());
    for (const glm::vec2& vertex : polygon.verticesXZ) {
        minPoint = glm::min(minPoint, vertex);
        maxPoint = glm::max(maxPoint, vertex);
    }

    std::uniform_real_distribution<float> xDist(minPoint.x, maxPoint.x);
    std::uniform_real_distribution<float> zDist(minPoint.y, maxPoint.y);
    for (int sampleIndex = 0; sampleIndex < 4096; ++sampleIndex) {
        const glm::vec2 candidate(xDist(rng), zDist(rng));
        if (!boxFootprintFitsInsidePolygon(candidate, polygon, halfExtentsXZ)) {
            continue;
        }
        return glm::vec3(candidate.x, polygon.elevationY, candidate.y);
    }
    return std::nullopt;
}

void witnessRandomBoxClearanceNavmeshHang() {
    core::NavMeshAsset asset{};
    std::string error{};
    const std::filesystem::path navmeshPath = assetPath("assets/navmeshes/DefaultScene.navmesh");
    assert(core::parseNavMeshAsset(readFileToString(navmeshPath), asset, &error));
    assert(error.empty());
    assert(asset.polygons.size() == 4u);

    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset = asset;
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    core::NavAgentComponent agent{};
    agent.clearanceSource = core::NavAgentClearanceSource::BoxCollider;
    world.navAgents.emplace(agentEntity, agent);
    const glm::vec3 boxHalfExtents(0.22f, 3.3f, 0.22f);
    world.boxColliders.emplace(agentEntity, core::BoxColliderComponent{glm::vec3(0.0f), boxHalfExtents, false, false});

    const std::size_t seed = envSizeTOr("ALKANZAR_WITNESS_NAV_SEED", 1337u);
    const std::size_t maxAttempts = envSizeTOr("ALKANZAR_WITNESS_NAV_ATTEMPTS", 50000u);
    const double slowThresholdMs = envDoubleOr("ALKANZAR_WITNESS_NAV_SLOW_MS", 100.0);
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    std::uniform_int_distribution<int> polygonDist(0, static_cast<int>(asset.polygons.size() - 1u));

    struct SlowCase {
        std::size_t attempt{0u};
        std::size_t startPolygonIndex{0u};
        std::size_t destinationPolygonIndex{0u};
        glm::vec3 start{0.0f};
        glm::vec3 destination{0.0f};
        double durationMs{0.0};
        bool solved{false};
        std::vector<glm::vec3> corners{};
    };

    std::vector<SlowCase> cases{};
    cases.reserve(maxAttempts);
    for (std::size_t attempt = 1u; attempt <= maxAttempts; ++attempt) {
        const std::size_t startPolygonIndex = static_cast<std::size_t>(polygonDist(rng));
        std::size_t destinationPolygonIndex = static_cast<std::size_t>(polygonDist(rng));
        if (asset.polygons.size() > 1u) {
            while (destinationPolygonIndex == startPolygonIndex) {
                destinationPolygonIndex = static_cast<std::size_t>(polygonDist(rng));
            }
        }

        const std::optional<glm::vec3> start = sampleRandomPointInsidePolygonForBox(
            asset.polygons[startPolygonIndex],
            glm::vec2(boxHalfExtents.x, boxHalfExtents.z),
            rng
        );
        const std::optional<glm::vec3> destination = sampleRandomPointInsidePolygonForBox(
            asset.polygons[destinationPolygonIndex],
            glm::vec2(boxHalfExtents.x, boxHalfExtents.z),
            rng
        );
        assert(start.has_value());
        assert(destination.has_value());

        world.transforms.get(agentEntity).position = *start;
        core::NavAgentComponent& navAgent = world.navAgents.get(agentEntity);
        navAgent.pathCorners.clear();
        navAgent.destination.reset();
        navAgent.moving = false;

        const auto startedAt = std::chrono::steady_clock::now();
        const bool solved = navigation.setAgentDestination(world, runtime, agentEntity, *destination);
        const double durationMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
            std::chrono::steady_clock::now() - startedAt).count();

        cases.push_back(SlowCase{
            attempt,
            startPolygonIndex,
            destinationPolygonIndex,
            *start,
            *destination,
            durationMs,
            solved,
            world.navAgents.get(agentEntity).pathCorners
        });
    }

    assert(!cases.empty());
    std::sort(cases.begin(), cases.end(), [](const SlowCase& lhs, const SlowCase& rhs) {
        return lhs.durationMs > rhs.durationMs;
    });

    const std::size_t reportCount = std::max<std::size_t>(1u, cases.size() / 10u);
    const std::size_t thresholdExceedCount = static_cast<std::size_t>(std::count_if(
        cases.begin(),
        cases.end(),
        [slowThresholdMs](const SlowCase& candidate) { return candidate.durationMs >= slowThresholdMs; }
    ));
    std::printf(
        "[navmesh-witness] seed=%zu attempts=%zu threshold_ms=%.3f threshold_exceed_count=%zu reporting_top=%zu\n",
        seed,
        maxAttempts,
        slowThresholdMs,
        thresholdExceedCount,
        reportCount
    );
    for (std::size_t reportIndex = 0; reportIndex < reportCount; ++reportIndex) {
        const SlowCase& slowCase = cases[reportIndex];
        std::printf(
            "[navmesh-witness rank=%zu] attempt=%zu duration_ms=%.3f solved=%d start_poly=%d dest_poly=%d corners=%zu\n",
            reportIndex + 1u,
            slowCase.attempt,
            slowCase.durationMs,
            slowCase.solved ? 1 : 0,
            asset.polygons[slowCase.startPolygonIndex].id,
            asset.polygons[slowCase.destinationPolygonIndex].id,
            slowCase.corners.size()
        );
        std::printf(
            "  start=(%.4f, %.4f, %.4f) dest=(%.4f, %.4f, %.4f)\n",
            slowCase.start.x, slowCase.start.y, slowCase.start.z,
            slowCase.destination.x, slowCase.destination.y, slowCase.destination.z
        );
        for (std::size_t cornerIndex = 0; cornerIndex < slowCase.corners.size(); ++cornerIndex) {
            const glm::vec3& corner = slowCase.corners[cornerIndex];
            std::printf("  corner[%zu]=(%.4f, %.4f, %.4f)\n", cornerIndex, corner.x, corner.y, corner.z);
        }
    }
}

void testNavigationOverlapCorridorConstraint() {
    // Two polygons form an L-shape connected only through a small overlap.
    // A = tall rectangle on the left, B = wide rectangle on the top-right.
    // The straight line from bottom-A to right-B would exit the walkable area,
    // so pathfinding must route through the overlap region.
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

void testNavigationPathfindingChoosesShortestGeometricCorridor() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(2.0f, 0.0f), glm::vec2(2.0f, 10.0f), glm::vec2(0.0f, 10.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(2.0f, 0.0f), glm::vec2(8.0f, 0.0f), glm::vec2(8.0f, 2.0f), glm::vec2(2.0f, 2.0f)}},
        core::NavPolygon{3, 0.0f, {glm::vec2(2.0f, 8.0f), glm::vec2(8.0f, 8.0f), glm::vec2(8.0f, 10.0f), glm::vec2(2.0f, 10.0f)}},
        core::NavPolygon{4, 0.0f, {glm::vec2(8.0f, 0.0f), glm::vec2(10.0f, 0.0f), glm::vec2(10.0f, 10.0f), glm::vec2(8.0f, 10.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    const glm::vec3 start(1.0f, 0.0f, 7.0f);
    const glm::vec3 destination(9.0f, 0.0f, 6.0f);
    world.transforms.emplace(agentEntity, core::TransformComponent{start, glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    const double actual = pathLength(start, agent.pathCorners);
    const double expectedUpperRoute =
        glm::distance(start, glm::vec3(2.0f, 0.0f, 8.0f)) +
        glm::distance(glm::vec3(2.0f, 0.0f, 8.0f), glm::vec3(8.0f, 0.0f, 8.0f)) +
        glm::distance(glm::vec3(8.0f, 0.0f, 8.0f), destination);

    assert(!agent.pathCorners.empty());
    assert(actual <= expectedUpperRoute + 0.01);
    assert(std::any_of(agent.pathCorners.begin(), agent.pathCorners.end(), [](const glm::vec3& corner) {
        return corner.z >= 8.0f - 0.01f;
    }));
    glm::vec3 previous = start;
    for (const glm::vec3& corner : agent.pathCorners) {
        assert(segmentInsidePolygons(previous, corner, runtime.asset.polygons));
        previous = corner;
    }

    core::NavAgentComponent& clearanceAgent = world.navAgents.get(agentEntity);
    clearanceAgent.clearanceSource = core::NavAgentClearanceSource::BoxCollider;
    world.boxColliders.emplace(agentEntity, core::BoxColliderComponent{
        glm::vec3(0.0f),
        glm::vec3(0.25f, 0.5f, 0.25f),
        false,
        false
    });
    assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
    const double clearancePathLength = pathLength(start, clearanceAgent.pathCorners);
    assert(clearancePathLength < 13.0);
    assert(std::any_of(
        clearanceAgent.pathCorners.begin(),
        clearanceAgent.pathCorners.end(),
        [](const glm::vec3& corner) { return corner.z >= 8.2f; }
    ));
    previous = start;
    for (const glm::vec3& corner : clearanceAgent.pathCorners) {
        assert(segmentInsidePolygons(previous, corner, runtime.asset.polygons));
        previous = corner;
    }
}

void testNavigationPathfindingKeepsShortestCorridorPastSixtyFourBoundaryNodes() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(2.0f, 0.0f), glm::vec2(2.0f, 10.0f), glm::vec2(0.0f, 10.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(2.0f, 0.0f), glm::vec2(8.0f, 0.0f), glm::vec2(8.0f, 2.0f), glm::vec2(2.0f, 2.0f)}},
        core::NavPolygon{3, 0.0f, {glm::vec2(2.0f, 8.0f), glm::vec2(8.0f, 8.0f), glm::vec2(8.0f, 10.0f), glm::vec2(2.0f, 10.0f)}},
        core::NavPolygon{4, 0.0f, {glm::vec2(8.0f, 0.0f), glm::vec2(10.0f, 0.0f), glm::vec2(10.0f, 10.0f), glm::vec2(8.0f, 10.0f)}},
    };

    // A connected dead-end branch contributes 82 exposed boundary vertices.
    // The old 64-node visibility cutoff therefore abandoned geometric search
    // and returned the center-selected lower corridor.
    constexpr int kBranchCellCount = 40;
    for (int branchIndex = 0; branchIndex < kBranchCellCount; ++branchIndex) {
        const float right = -static_cast<float>(branchIndex);
        const float left = right - 1.0f;
        runtime.asset.polygons.push_back(core::NavPolygon{
            100 + branchIndex,
            0.0f,
            {
                glm::vec2(left, 4.0f),
                glm::vec2(right, 4.0f),
                glm::vec2(right, 5.0f),
                glm::vec2(left, 5.0f),
            }
        });
    }
    assert(navigation.rebuildRuntime(runtime));
    assert(runtime.bakedCells.size() >= static_cast<std::size_t>(kBranchCellCount + 4));

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    const glm::vec3 start(1.0f, 0.0f, 7.0f);
    const glm::vec3 destination(9.0f, 0.0f, 6.0f);
    world.transforms.emplace(agentEntity, core::TransformComponent{
        start,
        glm::vec3(0.0f),
        glm::vec3(1.0f)
    });
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    const double expectedUpperRoute =
        glm::distance(start, glm::vec3(2.0f, 0.0f, 8.0f)) +
        glm::distance(glm::vec3(2.0f, 0.0f, 8.0f), glm::vec3(8.0f, 0.0f, 8.0f)) +
        glm::distance(glm::vec3(8.0f, 0.0f, 8.0f), destination);

    assert(nearlyEqual(pathLength(start, agent.pathCorners), expectedUpperRoute, 0.01));
    assert(std::any_of(agent.pathCorners.begin(), agent.pathCorners.end(), [](const glm::vec3& corner) {
        return corner.z >= 8.0f - 0.01f;
    }));
    assert(std::none_of(agent.pathCorners.begin(), agent.pathCorners.end(), [](const glm::vec3& corner) {
        return corner.x < -0.01f;
    }));
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

void testNavigationAgentClearanceRemainsOptIn() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(4.0f, 0.0f), glm::vec2(4.0f, 2.0f), glm::vec2(0.0f, 2.0f)}}
    };
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId entity = world.createEntity();
    world.transforms.emplace(entity, core::TransformComponent{glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(entity, core::NavAgentComponent{});
    world.sphereColliders.emplace(entity, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, false, false});

    assert(navigation.setAgentDestination(world, runtime, entity, glm::vec3(3.9f, 0.0f, 1.0f)));

    const core::NavAgentComponent& agent = world.navAgents.get(entity);
    assert(agent.destination.has_value());
    assert(nearlyEqual(agent.destination->x, 3.9, 0.001));
    assert(nearlyEqual(agent.pathCorners.back().x, 3.9, 0.001));
}

void testNavigationAgentSphereClearancePullsDestinationAwayFromWalls() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(4.0f, 0.0f), glm::vec2(4.0f, 2.0f), glm::vec2(0.0f, 2.0f)}}
    };
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId entity = world.createEntity();
    world.transforms.emplace(entity, core::TransformComponent{glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    core::NavAgentComponent agent{};
    agent.clearanceSource = core::NavAgentClearanceSource::SphereCollider;
    world.navAgents.emplace(entity, agent);
    world.sphereColliders.emplace(entity, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, false, false});

    assert(navigation.setAgentDestination(world, runtime, entity, glm::vec3(3.9f, 0.0f, 1.0f)));

    const core::NavAgentComponent& solvedAgent = world.navAgents.get(entity);
    assert(solvedAgent.destination.has_value());
    assert(solvedAgent.destination->x <= 3.5f + 0.02f);
    assert(solvedAgent.destination->x >= 3.5f - 0.05f);
    assert(solvedAgent.destination->z >= 0.5f - 0.02f);
    assert(solvedAgent.destination->z <= 1.5f + 0.02f);

    for (int step = 0; step < 16; ++step) {
        navigation.updateAgents(world, runtime, core::TimeContext{0.0f, 0.25f});
    }

    const core::TransformComponent& finalTransform = world.transforms.get(entity);
    const core::NavAgentComponent& finalAgent = world.navAgents.get(entity);
    assert(!finalAgent.moving);
    assert(!finalAgent.destination.has_value());
    assert(finalTransform.position.x <= 3.5f + 0.02f);
    assert(finalTransform.position.z >= 0.5f - 0.02f);
    assert(finalTransform.position.z <= 1.5f + 0.02f);
}

void testNavigationAgentSphereClearanceRejectsTooNarrowCorridor() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{
            1,
            0.0f,
            {
                glm::vec2(0.0f, 0.0f),
                glm::vec2(2.0f, 0.0f),
                glm::vec2(2.0f, 0.4f),
                glm::vec2(4.0f, 0.4f),
                glm::vec2(4.0f, 0.0f),
                glm::vec2(6.0f, 0.0f),
                glm::vec2(6.0f, 2.0f),
                glm::vec2(4.0f, 2.0f),
                glm::vec2(4.0f, 1.6f),
                glm::vec2(2.0f, 1.6f),
                glm::vec2(2.0f, 2.0f),
                glm::vec2(0.0f, 2.0f),
            }
        }
    };
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId entity = world.createEntity();
    world.transforms.emplace(entity, core::TransformComponent{glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    core::NavAgentComponent agent{};
    agent.clearanceSource = core::NavAgentClearanceSource::SphereCollider;
    world.navAgents.emplace(entity, agent);
    world.sphereColliders.emplace(entity, core::SphereColliderComponent{glm::vec3(0.0f), 0.7f, false, false});

    assert(!navigation.setAgentDestination(world, runtime, entity, glm::vec3(5.0f, 0.0f, 1.0f)));
    const core::NavAgentComponent& solvedAgent = world.navAgents.get(entity);
    assert(!solvedAgent.destination.has_value());
    assert(solvedAgent.pathCorners.empty());
}

void testNavigationAgentBoxClearanceUsesLateralFootprintAndKeepsDirectPath() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(10.0f, 0.0f), glm::vec2(10.0f, 1.0f), glm::vec2(0.0f, 1.0f)}}
    };
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId entity = world.createEntity();
    world.transforms.emplace(entity, core::TransformComponent{glm::vec3(1.0f, 0.0f, 0.5f), glm::vec3(0.0f, 45.0f, 0.0f), glm::vec3(1.0f)});
    core::NavAgentComponent agent{};
    agent.clearanceSource = core::NavAgentClearanceSource::BoxCollider;
    world.navAgents.emplace(entity, agent);
    world.boxColliders.emplace(entity, core::BoxColliderComponent{glm::vec3(0.0f), glm::vec3(0.20f, 0.5f, 0.80f), false, false});

    assert(navigation.setAgentDestination(world, runtime, entity, glm::vec3(9.0f, 0.0f, 0.5f)));

    const core::NavAgentComponent& solvedAgent = world.navAgents.get(entity);
    assert(solvedAgent.destination.has_value());
    assert(nearlyEqual(solvedAgent.destination->x, 9.0, 0.001));
    assert(nearlyEqual(solvedAgent.destination->z, 0.5, 0.001));
    assert(solvedAgent.pathCorners.size() == 1u);
    assert(nearlyEqual(solvedAgent.pathCorners.front().x, 9.0, 0.001));
    assert(nearlyEqual(solvedAgent.pathCorners.front().z, 0.5, 0.001));
}

void testNavigationRotatedBoxClearancePreservesExplicitLinkTransitions() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{10, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f)}},
        core::NavPolygon{11, 3.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f)}},
    };
    runtime.asset.links = {
        core::NavLink{3, 10, 11, glm::vec3(0.5f, 0.0f, 0.5f), glm::vec3(0.5f, 3.0f, 0.5f), true}
    };
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId entity = world.createEntity();
    world.transforms.emplace(entity, core::TransformComponent{glm::vec3(0.2f, 0.0f, 0.5f), glm::vec3(0.0f, 45.0f, 0.0f), glm::vec3(1.0f)});
    core::NavAgentComponent agent{};
    agent.clearanceSource = core::NavAgentClearanceSource::BoxCollider;
    world.navAgents.emplace(entity, agent);
    world.boxColliders.emplace(entity, core::BoxColliderComponent{glm::vec3(0.0f), glm::vec3(0.15f, 0.5f, 0.35f), false, false});

    const glm::vec3 destination(0.5f, 3.0f, 0.5f);
    assert(navigation.setAgentDestination(world, runtime, entity, destination));

    const core::NavAgentComponent& solvedAgent = world.navAgents.get(entity);
    assert(solvedAgent.destination.has_value());
    assert(nearlyEqual(solvedAgent.destination->x, destination.x, 0.02));
    assert(nearlyEqual(solvedAgent.destination->y, destination.y, 0.02));
    assert(nearlyEqual(solvedAgent.destination->z, destination.z, 0.02));
    assert(solvedAgent.pathCorners.size() >= 2u);
    assert(std::any_of(
        solvedAgent.pathCorners.begin(),
        solvedAgent.pathCorners.end(),
        [](const glm::vec3& corner) {
            return nearlyEqual(corner.x, 0.5, 0.02) &&
                nearlyEqual(corner.y, 0.0, 0.02) &&
                nearlyEqual(corner.z, 0.5, 0.02);
        }
    ));
    assert(std::any_of(
        solvedAgent.pathCorners.begin(),
        solvedAgent.pathCorners.end(),
        [](const glm::vec3& corner) {
            return nearlyEqual(corner.x, 0.5, 0.02) &&
                nearlyEqual(corner.y, 3.0, 0.02) &&
                nearlyEqual(corner.z, 0.5, 0.02);
        }
    ));
    assert(nearlyEqual(solvedAgent.pathCorners.back().x, destination.x, 0.02));
    assert(nearlyEqual(solvedAgent.pathCorners.back().y, destination.y, 0.02));
    assert(nearlyEqual(solvedAgent.pathCorners.back().z, destination.z, 0.02));
}

void testNavigationClearanceResolvesTowardApproachInsteadOfCornerVertex() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(4.0f, 0.0f), glm::vec2(4.0f, 4.0f), glm::vec2(0.0f, 4.0f)}}
    };
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId entity = world.createEntity();
    const glm::vec3 start(0.5f, 0.0f, 2.0f);
    const glm::vec3 click(3.9f, 0.0f, 0.1f);
    world.transforms.emplace(entity, core::TransformComponent{start, glm::vec3(0.0f), glm::vec3(1.0f)});
    core::NavAgentComponent agent{};
    agent.clearanceSource = core::NavAgentClearanceSource::SphereCollider;
    world.navAgents.emplace(entity, agent);
    world.sphereColliders.emplace(entity, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, false, false});

    assert(navigation.setAgentDestination(world, runtime, entity, click));

    const core::NavAgentComponent& solvedAgent = world.navAgents.get(entity);
    assert(solvedAgent.destination.has_value());
    assert(solvedAgent.pathCorners.size() == 1u);
    assert(nearlyEqual(solvedAgent.destination->z, 0.5, 0.02));
    assert(nearlyEqual(solvedAgent.destination->x, 3.1842, 0.05));
    assert(solvedAgent.destination->x < 3.35f);
    assert(nearlyEqual(solvedAgent.pathCorners.front().x, solvedAgent.destination->x, 0.001));
    assert(nearlyEqual(solvedAgent.pathCorners.front().z, solvedAgent.destination->z, 0.001));
}

void testNavigationOverlapLShapeUsesShortcutThroughOverlapRegion() {
    // Two rectangles forming an L-shape connected through a 2D overlap region.
    // A = tall rectangle on the left, B = wide rectangle on the top-right.
    // Overlap region is a square at their intersection.
    // The agent must route through the overlap; the path should be close to the
    // Euclidean shortest route within the walkable union.
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(3.0f, 0.0f), glm::vec2(3.0f, 5.0f), glm::vec2(0.0f, 5.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(2.0f, 4.0f), glm::vec2(8.0f, 4.0f), glm::vec2(8.0f, 7.0f), glm::vec2(2.0f, 7.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));
    assert(!runtime.bakedCells.empty());

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    const glm::vec3 start(1.0f, 0.0f, 1.0f);
    const glm::vec3 destination(7.0f, 0.0f, 6.0f);
    world.transforms.emplace(agentEntity, core::TransformComponent{start, glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(!agent.pathCorners.empty());

    const double actual = pathLength(start, agent.pathCorners);
    // The optimal geometric path goes through the overlap corner region.
    // Compute a reasonable upper bound: the straight-line Euclidean distance plus
    // a small tolerance for routing around the L-bend.
    const double euclidean = glm::distance(start, destination);
    const double tolerance = 1.5;

    std::printf("[L-shape overlap] cells=%zu corners=%zu pathLength=%.4f euclidean=%.4f ratio=%.4f\n",
        runtime.bakedCells.size(),
        agent.pathCorners.size(),
        actual,
        euclidean,
        actual / euclidean);

    // Every segment must stay inside the walkable union.
    glm::vec3 previous = start;
    for (const glm::vec3& corner : agent.pathCorners) {
        assert(segmentInsidePolygons(previous, corner, runtime.asset.polygons));
        previous = corner;
    }

    // Path must not be dramatically longer than Euclidean distance.
    assert(actual < euclidean + tolerance);
}

void testNavigationOverlapTShapeRoutesThroughWideOverlap() {
    // Two rectangles: a long horizontal bar and a vertical bar overlapping
    // at the center, forming a T-shape. The overlap is a wide 2D region.
    // Agent travels from the left end to the top of the vertical bar.
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 2.0f), glm::vec2(10.0f, 2.0f), glm::vec2(10.0f, 4.0f), glm::vec2(0.0f, 4.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(4.0f, 0.0f), glm::vec2(6.0f, 0.0f), glm::vec2(6.0f, 4.0f), glm::vec2(4.0f, 4.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));
    assert(!runtime.bakedCells.empty());

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    const glm::vec3 start(1.0f, 0.0f, 3.0f);
    const glm::vec3 destination(5.0f, 0.0f, 0.5f);
    world.transforms.emplace(agentEntity, core::TransformComponent{start, glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(!agent.pathCorners.empty());

    const double actual = pathLength(start, agent.pathCorners);
    const double euclidean = glm::distance(start, destination);

    std::printf("[T-shape overlap] cells=%zu corners=%zu pathLength=%.4f euclidean=%.4f ratio=%.4f\n",
        runtime.bakedCells.size(),
        agent.pathCorners.size(),
        actual,
        euclidean,
        actual / euclidean);
    for (std::size_t i = 0; i < agent.pathCorners.size(); ++i) {
        std::printf("  corner[%zu]=(%.4f, %.4f, %.4f)\n", i,
            agent.pathCorners[i].x, agent.pathCorners[i].y, agent.pathCorners[i].z);
    }

    glm::vec3 previous = start;
    for (const glm::vec3& corner : agent.pathCorners) {
        assert(segmentInsidePolygons(previous, corner, runtime.asset.polygons));
        previous = corner;
    }

    // The direct line exits the walkable union between the two polygons,
    // so a small detour through the overlap is expected. The ratio should
    // stay below 1.15 (within 15% of Euclidean).
    assert(actual / euclidean < 1.15);
}

void testNavigationOverlapDiagonalCrossing() {
    // Two rectangles with a square overlap region in the middle. Agent crosses
    // diagonally from the non-overlap part of polygon A to the non-overlap
    // part of polygon B, passing through the 2D overlap.
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(4.0f, 0.0f), glm::vec2(4.0f, 4.0f), glm::vec2(0.0f, 4.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(3.0f, 3.0f), glm::vec2(7.0f, 3.0f), glm::vec2(7.0f, 7.0f), glm::vec2(3.0f, 7.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));
    assert(!runtime.bakedCells.empty());

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    const glm::vec3 start(1.0f, 0.0f, 1.0f);
    const glm::vec3 destination(6.0f, 0.0f, 6.0f);
    world.transforms.emplace(agentEntity, core::TransformComponent{start, glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(!agent.pathCorners.empty());

    const double actual = pathLength(start, agent.pathCorners);
    const double euclidean = glm::distance(start, destination);

    std::printf("[diagonal crossing] cells=%zu corners=%zu pathLength=%.4f euclidean=%.4f ratio=%.4f\n",
        runtime.bakedCells.size(),
        agent.pathCorners.size(),
        actual,
        euclidean,
        actual / euclidean);

    glm::vec3 previous = start;
    for (const glm::vec3& corner : agent.pathCorners) {
        assert(segmentInsidePolygons(previous, corner, runtime.asset.polygons));
        previous = corner;
    }

    // The direct line goes outside the walkable union (clips the gap between
    // polygon corners), so the path routes through the overlap corner.
    // It must still be reasonably close to Euclidean (ratio < 1.3).
    assert(actual / euclidean < 1.3);
}

void testNavigationOverlapFullContainmentUsesStraightPath() {
    // One small polygon fully inside a larger one. The shared region is the
    // entire smaller polygon (2D). The agent should get a direct straight path.
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(10.0f, 0.0f), glm::vec2(10.0f, 10.0f), glm::vec2(0.0f, 10.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(3.0f, 3.0f), glm::vec2(7.0f, 3.0f), glm::vec2(7.0f, 7.0f), glm::vec2(3.0f, 7.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));
    assert(!runtime.bakedCells.empty());

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    const glm::vec3 start(1.0f, 0.0f, 5.0f);
    const glm::vec3 destination(9.0f, 0.0f, 5.0f);
    world.transforms.emplace(agentEntity, core::TransformComponent{start, glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(!agent.pathCorners.empty());

    const double actual = pathLength(start, agent.pathCorners);
    const double euclidean = glm::distance(start, destination);

    std::printf("[full containment] cells=%zu corners=%zu pathLength=%.4f euclidean=%.4f ratio=%.4f\n",
        runtime.bakedCells.size(),
        agent.pathCorners.size(),
        actual,
        euclidean,
        actual / euclidean);

    // Straight line stays entirely inside the outer polygon, so path must
    // equal the Euclidean distance.
    assert(nearlyEqual(actual, euclidean, 0.01));
    assert(agent.pathCorners.size() == 1u);
}

void testNavigationOverlapThreePolygonChain() {
    // Three overlapping rectangles in a chain: A overlaps B, B overlaps C.
    // Agent goes from polygon A to polygon C through two overlap regions.
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {glm::vec2(0.0f, 0.0f), glm::vec2(3.0f, 0.0f), glm::vec2(3.0f, 2.0f), glm::vec2(0.0f, 2.0f)}},
        core::NavPolygon{2, 0.0f, {glm::vec2(2.0f, 0.0f), glm::vec2(5.0f, 0.0f), glm::vec2(5.0f, 2.0f), glm::vec2(2.0f, 2.0f)}},
        core::NavPolygon{3, 0.0f, {glm::vec2(4.0f, 0.0f), glm::vec2(7.0f, 0.0f), glm::vec2(7.0f, 2.0f), glm::vec2(4.0f, 2.0f)}},
    };
    assert(navigation.rebuildRuntime(runtime));
    assert(!runtime.bakedCells.empty());

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    const glm::vec3 start(0.5f, 0.0f, 1.0f);
    const glm::vec3 destination(6.5f, 0.0f, 1.0f);
    world.transforms.emplace(agentEntity, core::TransformComponent{start, glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    assert(navigation.setAgentDestination(world, runtime, agentEntity, destination));
    const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
    assert(!agent.pathCorners.empty());

    const double actual = pathLength(start, agent.pathCorners);
    const double euclidean = glm::distance(start, destination);

    std::printf("[3-polygon chain] cells=%zu corners=%zu pathLength=%.4f euclidean=%.4f ratio=%.4f\n",
        runtime.bakedCells.size(),
        agent.pathCorners.size(),
        actual,
        euclidean,
        actual / euclidean);

    // Straight horizontal line through all three polygons.
    assert(nearlyEqual(actual, euclidean, 0.01));
    assert(agent.pathCorners.size() == 1u);
}

void testNavigationDefaultSceneNavmeshBoxClearanceOverlap() {
    // Real-world navmesh: three irregular quadrilaterals with thin 2D overlap
    // strips near y ≈ 1.2–1.7. An agent with a box collider (0.22 lateral
    // half-extent) pathfinds between all polygon pairs. The portals through
    // the overlap strips are narrow, so this exercises the sharedPortal fix
    // and shrinkPortal clearance logic.
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {
            glm::vec2(0.3140f, 4.2004f), glm::vec2(-0.6911f, 1.2521f),
            glm::vec2(-12.5968f, 1.1906f), glm::vec2(-10.7201f, 3.9619f)
        }},
        core::NavPolygon{2, 0.0f, {
            glm::vec2(-12.1498f, 1.5451f), glm::vec2(-9.5003f, 1.7270f),
            glm::vec2(-6.6857f, -5.5753f), glm::vec2(-9.6089f, -7.7274f)
        }},
        core::NavPolygon{3, 0.0f, {
            glm::vec2(-3.0140f, 1.7051f), glm::vec2(-0.8352f, 1.6632f),
            glm::vec2(-7.4791f, -5.6901f), glm::vec2(-8.1356f, -3.3239f)
        }},
    };
    assert(navigation.rebuildRuntime(runtime));
    assert(!runtime.bakedCells.empty());

    std::printf("[defaultscene] baked %zu cells, %zu graph edges total\n",
        runtime.bakedCells.size(),
        [&]() {
            std::size_t total = 0;
            for (const auto& edges : runtime.graph) total += edges.size();
            return total;
        }());

    // Points well inside each polygon (verified numerically).
    const glm::vec3 pointInPoly1(-5.5f, 0.0f, 2.8f);
    const glm::vec3 pointInPoly2(-9.5f, 0.0f, -2.0f);
    const glm::vec3 pointInPoly3(-4.5f, 0.0f, -1.0f);

    struct TestCase {
        const char* label;
        glm::vec3 start;
        glm::vec3 dest;
    };
    const TestCase cases[] = {
        {"P1->P2", pointInPoly1, pointInPoly2},
        {"P2->P1", pointInPoly2, pointInPoly1},
        {"P1->P3", pointInPoly1, pointInPoly3},
        {"P3->P1", pointInPoly3, pointInPoly1},
        {"P2->P3", pointInPoly2, pointInPoly3},
        {"P3->P2", pointInPoly3, pointInPoly2},
    };

    for (const TestCase& tc : cases) {
        core::World world;
        const core::EntityId entity = world.createEntity();
        world.transforms.emplace(entity, core::TransformComponent{
            tc.start, glm::vec3(0.0f), glm::vec3(1.0f)});
        core::NavAgentComponent agent{};
        agent.clearanceSource = core::NavAgentClearanceSource::BoxCollider;
        world.navAgents.emplace(entity, agent);
        world.boxColliders.emplace(entity, core::BoxColliderComponent{
            glm::vec3(0.0f), glm::vec3(0.22f, 3.3f, 0.22f), false, false});

        const bool solved = navigation.setAgentDestination(world, runtime, entity, tc.dest);
        const core::NavAgentComponent& result = world.navAgents.get(entity);

        const double euclidean = glm::distance(tc.start, tc.dest);
        const double actual = solved ? pathLength(tc.start, result.pathCorners) : -1.0;
        const double ratio = solved ? actual / euclidean : -1.0;

        std::printf("[defaultscene %s] solved=%d corners=%zu pathLength=%.4f euclidean=%.4f ratio=%.4f\n",
            tc.label,
            solved ? 1 : 0,
            result.pathCorners.size(),
            actual,
            euclidean,
            ratio);
        std::printf("  start=(%.4f, %.4f, %.4f)\n", tc.start.x, tc.start.y, tc.start.z);
        for (std::size_t ci = 0; ci < result.pathCorners.size(); ++ci) {
            const glm::vec3& c = result.pathCorners[ci];
            std::printf("  corner[%zu]=(%.4f, %.4f, %.4f)\n", ci, c.x, c.y, c.z);
        }

        assert(solved);

        if (result.pathCorners.empty()) {
            // Clearance resolution collapsed start/dest — skip this direction.
            std::printf("  (skipped: clearance collapsed path)\n");
            continue;
        }

        if (result.destination.has_value()) {
            const glm::vec3& d = *result.destination;
            std::printf("  resolvedDest=(%.4f, %.4f, %.4f)\n", d.x, d.y, d.z);
        }

        // Every segment must stay inside the walkable union.
        glm::vec3 previous = tc.start;
        for (const glm::vec3& corner : result.pathCorners) {
            assert(segmentInsidePolygons(previous, corner, runtime.asset.polygons));
            previous = corner;
        }

        // Path must not be dramatically longer than Euclidean distance.
        // With clearance + routing through narrow overlaps, up to 70% overhead
        // is tolerable when the agent must detour to the widest part of a thin
        // overlap strip. Anything beyond that indicates a broken detour.
        assert(ratio < 1.7);
    }
}

void testNavigationClearancePathfindingAvoidsIntervalStateExplosion() {
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{1, 0.0f, {
            glm::vec2(0.3140f, 4.2004f), glm::vec2(-0.6911f, 1.2521f),
            glm::vec2(-12.5968f, 1.1906f), glm::vec2(-10.7201f, 3.9619f)
        }},
        core::NavPolygon{2, 0.0f, {
            glm::vec2(-12.1498f, 1.5451f), glm::vec2(-9.5003f, 1.7270f),
            glm::vec2(-6.6857f, -5.5753f), glm::vec2(-9.6089f, -7.7274f)
        }},
        core::NavPolygon{3, 0.0f, {
            glm::vec2(-3.0140f, 1.7051f), glm::vec2(-0.8352f, 1.6632f),
            glm::vec2(-7.4791f, -5.6901f), glm::vec2(-8.1356f, -3.3239f)
        }},
        core::NavPolygon{4, 0.0f, {
            glm::vec2(-0.4535f, 3.5254f), glm::vec2(-2.3404f, 0.4973f),
            glm::vec2(5.2315f, -0.2897f), glm::vec2(7.2602f, 2.7569f)
        }},
    };
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId entity = world.createEntity();
    const glm::vec3 start(0.3298f, 0.0f, 1.7822f);
    const glm::vec3 destination(-9.3173f, 0.0f, -2.1293f);
    world.transforms.emplace(entity, core::TransformComponent{
        start,
        glm::vec3(0.0f),
        glm::vec3(1.0f)
    });
    core::NavAgentComponent agent{};
    agent.clearanceSource = core::NavAgentClearanceSource::BoxCollider;
    world.navAgents.emplace(entity, agent);
    world.boxColliders.emplace(entity, core::BoxColliderComponent{
        glm::vec3(0.0f),
        glm::vec3(0.22f, 3.3f, 0.22f),
        false,
        false
    });

    const auto startedAt = std::chrono::steady_clock::now();
    const bool solved = navigation.setAgentDestination(world, runtime, entity, destination);
    const double durationSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::steady_clock::now() - startedAt
    ).count();
    const core::NavAgentComponent& solvedAgent = world.navAgents.get(entity);

    assert(solved);
    assert(durationSeconds < 1.0 / 60.0);
    assert(!solvedAgent.pathCorners.empty());
    assert(pathLength(start, solvedAgent.pathCorners) < 13.0);
    glm::vec3 previous = start;
    for (const glm::vec3& corner : solvedAgent.pathCorners) {
        assert(segmentInsidePolygons(previous, corner, runtime.asset.polygons));
        previous = corner;
    }
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
    world.renderables.emplace(childA, core::RenderableComponent{render::MeshHandle{0}, render::RenderLayer::Geometry});
    world.renderables.emplace(childB, core::RenderableComponent{render::MeshHandle{1}, render::RenderLayer::Geometry});
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
    world.renderables.emplace(childA, core::RenderableComponent{render::MeshHandle{0}, render::RenderLayer::Geometry});
    world.renderables.emplace(childB, core::RenderableComponent{render::MeshHandle{1}, render::RenderLayer::Geometry});
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

void testTransformMathBuildsRotatedBoxesWithoutInflatingLocalExtents() {
    const core::TransformComponent transform{
        glm::vec3(2.0f, 1.0f, -3.0f),
        glm::vec3(0.0f, 45.0f, 0.0f),
        glm::vec3(1.0f)
    };
    const core::BoxColliderComponent collider{glm::vec3(0.0f), glm::vec3(0.5f, 0.75f, 0.25f), false, false};

    const core::OrientedBox box = core::makeOrientedBox(transform, collider);

    assert(nearlyEqual(box.center.x, 2.0, 0.0001));
    assert(nearlyEqual(box.center.y, 1.0, 0.0001));
    assert(nearlyEqual(box.center.z, -3.0, 0.0001));
    assert(nearlyEqual(box.halfExtents.x, 0.5, 0.0001));
    assert(nearlyEqual(box.halfExtents.y, 0.75, 0.0001));
    assert(nearlyEqual(box.halfExtents.z, 0.25, 0.0001));
    assert(nearlyEqual(glm::length(box.axes[0]), 1.0, 0.0001));
    assert(nearlyEqual(glm::length(box.axes[1]), 1.0, 0.0001));
    assert(nearlyEqual(glm::length(box.axes[2]), 1.0, 0.0001));
    assert(std::abs(box.axes[0].x) > 0.6f);
    assert(std::abs(box.axes[0].z) > 0.6f);
    assert(std::abs(box.axes[2].x) > 0.6f);
    assert(std::abs(box.axes[2].z) > 0.6f);
    assert(nearlyEqual(glm::length(glm::vec3(box.modelMatrix[0])), 0.5, 0.0001));
    assert(nearlyEqual(glm::length(glm::vec3(box.modelMatrix[1])), 0.75, 0.0001));
    assert(nearlyEqual(glm::length(glm::vec3(box.modelMatrix[2])), 0.25, 0.0001));
}

void testLightVolumeAssignment() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::LightSystem lightSystem;
    core::RenderExtractionSystem extraction;
    const core::EntityId lightVolume = addLightVolumeEntity(world, glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(10.0f));
    const core::EntityId point = world.createEntity();
    const core::EntityId spot = world.createEntity();

    world.transforms.emplace(point, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(spot, core::TransformComponent{glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.pointLights.emplace(point, core::PointLightComponent{4.0f, glm::vec3(1.0f), 1.0f, 0.0f, false, false, 0.0f, 0.0f});
    world.spotLights.emplace(spot, core::SpotLightComponent{5.0f, glm::vec3(1.0f), 1.0f, glm::vec3(0.0f), 15.0f, 25.0f, 0.0f, true, false, 0.0f, 0.0f});
    world.markTransformsDirty(point);
    world.markTransformsDirty(spot);

    transformSystem.update(world, scheduler);
    lightSystem.update(world, core::TimeContext{1.0f, 0.016f}, scheduler);

    core::FrameSceneData frame;
    extraction.extract(world, selection, frame, scheduler);

    assert(frame.lightVolumes.size() == 1u);
    assert(frame.lightVolumes[0].entity == lightVolume);
    assert(frame.lightVolumes[0].staticLightIndices.size() == 1u);
    assert(frame.lightVolumes[0].movableLightIndices.size() == 1u);
    assert(frame.lights[frame.lightVolumes[0].staticLightIndices[0]].entity == point);
    assert(frame.lights[frame.lightVolumes[0].movableLightIndices[0]].entity == spot);
    assert(nearlyEqual(frame.lightVolumes[0].minCorner.x, -8.0, 0.0001));
    assert(nearlyEqual(frame.lightVolumes[0].maxCorner.x, 12.0, 0.0001));
}

void testLightVolumeAssignmentIsStableAcrossLightTypes() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::LightSystem lightSystem;
    core::RenderExtractionSystem extraction;
    addLightVolumeEntity(world, glm::vec3(0.0f), glm::vec3(10.0f));

    const core::EntityId spot = world.createEntity();
    const core::EntityId point = world.createEntity();
    world.transforms.emplace(spot, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(point, core::TransformComponent{glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.spotLights.emplace(spot, core::SpotLightComponent{5.0f, glm::vec3(1.0f), 1.0f, glm::vec3(0.0f), 15.0f, 25.0f, 0.0f, false, false, 0.0f, 0.0f});
    world.pointLights.emplace(point, core::PointLightComponent{4.0f, glm::vec3(1.0f), 1.0f, 0.0f, false, false, 0.0f, 0.0f});
    world.markTransformsDirty(spot);
    world.markTransformsDirty(point);

    transformSystem.update(world, scheduler);
    lightSystem.update(world, core::TimeContext{1.0f, 0.016f}, scheduler);

    core::FrameSceneData frame;
    extraction.extract(world, selection, frame, scheduler);

    assert(frame.lights.size() == 2u);
    assert(frame.lights[0].entity == point);
    assert(frame.lights[1].entity == spot);
    assert(frame.lightVolumes.size() == 1u);
    assert(frame.lightVolumes[0].staticLightIndices.size() == 2u);
    assert(frame.lightVolumes[0].staticLightIndices[0] == 0);
    assert(frame.lightVolumes[0].staticLightIndices[1] == 1);
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

void testMaterialComponentSharingExtraction() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::SelectionModel selection;
    core::RenderExtractionSystem extraction;

    const std::shared_ptr<render::Material> material = makeMaterial("Shared");

    const core::EntityId a = world.createEntity();
    const core::EntityId b = world.createEntity();
    world.transforms.emplace(a, core::TransformComponent{});
    world.transforms.emplace(b, core::TransformComponent{});
    world.bounds.emplace(a, core::BoundsComponent{render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)}});
    world.bounds.emplace(b, core::BoundsComponent{render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)}});
    world.renderables.emplace(a, core::RenderableComponent{render::MeshHandle{0}, render::RenderLayer::Geometry});
    world.renderables.emplace(b, core::RenderableComponent{render::MeshHandle{1}, render::RenderLayer::Geometry});
    world.materials.emplace(a, core::MaterialComponent{material});
    world.materials.emplace(b, core::MaterialComponent{material});
    world.markTransformsDirty(a);
    world.markTransformsDirty(b);

    core::TransformSystem transformSystem;
    transformSystem.update(world, scheduler);

    core::FrameSceneData frame;
    extraction.extract(world, selection, frame, scheduler, true);

    assert(frame.renderables.size() == 2u);
    assert(frame.renderables[0].material == frame.renderables[1].material);
    assert(frame.renderables[0].material == material);
}

void testRenderExtractionDefaultsMissingVisibilityToHidden() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::RenderExtractionSystem extraction;

    const core::EntityId hidden = world.createEntity();
    const core::EntityId visible = world.createEntity();
    world.transforms.emplace(hidden, core::TransformComponent{});
    world.transforms.emplace(visible, core::TransformComponent{glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.bounds.emplace(hidden, core::BoundsComponent{render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)}});
    world.bounds.emplace(visible, core::BoundsComponent{render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)}});
    world.renderables.emplace(hidden, core::RenderableComponent{render::MeshHandle{0}, render::RenderLayer::Geometry});
    world.renderables.emplace(visible, core::RenderableComponent{render::MeshHandle{1}, render::RenderLayer::Geometry});
    world.visibilities.emplace(visible, core::VisibilityComponent{true});
    world.markTransformsDirty(hidden);
    world.markTransformsDirty(visible);

    transformSystem.update(world, scheduler);

    core::FrameSceneData frame;
    extraction.extract(world, selection, frame, scheduler);

    assert(frame.renderables.size() == 2u);
    assert(frame.renderables[0].entity == hidden);
    assert(!frame.renderables[0].visible);
    assert(frame.renderables[1].entity == visible);
    assert(frame.renderables[1].visible);
}

void testRenderExtractionTracksFocusedComponentSelection() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::RenderExtractionSystem extraction;

    const core::EntityId lightVolume = addLightVolumeEntity(world, glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(2.0f));
    transformSystem.update(world, scheduler);

    core::FrameSceneData frame;
    selection.set(std::optional<core::SelectionTarget>{core::SelectionTarget{lightVolume, core::ComponentKind::LightVolume}});
    extraction.extract(world, selection, frame, scheduler);

    assert(frame.selection.entity.has_value());
    assert(*frame.selection.entity == lightVolume);
    assert(frame.selection.component.has_value());
    assert(*frame.selection.component == core::ComponentKind::LightVolume);
    assert(frame.selection.hasWorldBounds);
    assert(nearlyEqual(frame.selection.worldBounds.min.x, 1.0, 0.0001));
    assert(nearlyEqual(frame.selection.worldBounds.max.x, 5.0, 0.0001));
}

void testRenderExtractionEmitsVisibleColliderDebugOnly() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::RenderExtractionSystem extraction;

    const core::EntityId box = world.createEntity();
    world.transforms.emplace(box, core::TransformComponent{glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.boxColliders.emplace(box, core::BoxColliderComponent{glm::vec3(0.0f), glm::vec3(0.5f), false, true});
    world.visibilities.emplace(box, core::VisibilityComponent{true});

    const core::EntityId sphere = world.createEntity();
    world.transforms.emplace(sphere, core::TransformComponent{glm::vec3(-2.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.sphereColliders.emplace(sphere, core::SphereColliderComponent{glm::vec3(0.0f), 0.75f, false, true});
    world.visibilities.emplace(sphere, core::VisibilityComponent{true});

    const core::EntityId hidden = world.createEntity();
    world.transforms.emplace(hidden, core::TransformComponent{glm::vec3(4.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.boxColliders.emplace(hidden, core::BoxColliderComponent{glm::vec3(0.0f), glm::vec3(0.5f), false, true});

    const core::EntityId disabled = world.createEntity();
    world.transforms.emplace(disabled, core::TransformComponent{glm::vec3(6.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.sphereColliders.emplace(disabled, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, false, false});
    world.visibilities.emplace(disabled, core::VisibilityComponent{true});

    world.markTransformsDirty(box);
    world.markTransformsDirty(sphere);
    world.markTransformsDirty(hidden);
    world.markTransformsDirty(disabled);
    transformSystem.update(world, scheduler);

    core::FrameSceneData frame;
    extraction.extract(world, selection, frame, scheduler);

    assert(frame.colliderDebug.size() == 2u);
    assert(frame.colliderDebug[0].entity == box);
    assert(frame.colliderDebug[0].shape == core::FrameColliderShape::Box);
    assert(nearlyEqual(frame.colliderDebug[0].bounds.min.x, 0.5, 0.0001));
    assert(nearlyEqual(frame.colliderDebug[0].bounds.max.x, 1.5, 0.0001));
    assert(frame.colliderDebug[1].entity == sphere);
    assert(frame.colliderDebug[1].shape == core::FrameColliderShape::Sphere);
    assert(nearlyEqual(frame.colliderDebug[1].center.x, -2.0, 0.0001));
    assert(nearlyEqual(frame.colliderDebug[1].radius, 0.75, 0.0001));
}

void testRenderExtractionUsesOrientedBoxMatricesForRotatedColliders() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::RenderExtractionSystem extraction;

    const core::EntityId box = world.createEntity();
    world.transforms.emplace(box, core::TransformComponent{glm::vec3(1.0f, 0.0f, 2.0f), glm::vec3(0.0f, 45.0f, 0.0f), glm::vec3(1.0f)});
    world.boxColliders.emplace(box, core::BoxColliderComponent{glm::vec3(0.0f), glm::vec3(0.5f, 1.0f, 0.25f), false, true});
    world.visibilities.emplace(box, core::VisibilityComponent{true});
    world.markTransformsDirty(box);
    transformSystem.update(world, scheduler);

    selection.set(box);

    core::FrameSceneData frame;
    extraction.extract(world, selection, frame, scheduler);

    assert(frame.colliderDebug.size() == 1u);
    assert(frame.colliderDebug.front().entity == box);
    assert(frame.colliderDebug.front().shape == core::FrameColliderShape::Box);
    assert(nearlyEqual(glm::length(glm::vec3(frame.colliderDebug.front().modelMatrix[0])), 0.5, 0.0001));
    assert(nearlyEqual(glm::length(glm::vec3(frame.colliderDebug.front().modelMatrix[1])), 1.0, 0.0001));
    assert(nearlyEqual(glm::length(glm::vec3(frame.colliderDebug.front().modelMatrix[2])), 0.25, 0.0001));
    assert(std::abs(frame.colliderDebug.front().modelMatrix[0].z) > 0.3f);
    assert(frame.selection.hasBoundsModelMatrix);
    assert(nearlyEqual(glm::length(glm::vec3(frame.selection.boundsModelMatrix[0])), 0.5, 0.0001));
    assert(nearlyEqual(glm::length(glm::vec3(frame.selection.boundsModelMatrix[1])), 1.0, 0.0001));
    assert(nearlyEqual(glm::length(glm::vec3(frame.selection.boundsModelMatrix[2])), 0.25, 0.0001));
    assert(std::abs(frame.selection.boundsModelMatrix[0].z) > 0.3f);

    const render::RenderSceneView scene = render::buildRenderSceneView(frame, {}, scheduler);
    assert(scene.selection.hasBoundsModelMatrix);
    assert(nearlyEqual(glm::length(glm::vec3(scene.selection.boundsModelMatrix[0])), 0.5, 0.0001));
    assert(std::abs(scene.selection.boundsModelMatrix[0].z) > 0.3f);
}

void testRenderExtractionPreservesOutputOrdering() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler();
    core::SelectionModel selection;
    core::RenderExtractionSystem extraction;
    core::TransformSystem transformSystem;
    core::LightSystem lightSystem;

    const std::shared_ptr<render::Material> material = makeMaterial();

    const core::EntityId renderableA = world.createEntity();
    const core::EntityId point = world.createEntity();
    const core::EntityId renderableB = world.createEntity();
    const core::EntityId spot = world.createEntity();

    addLightVolumeEntity(world, glm::vec3(0.0f), glm::vec3(10.0f));
    world.transforms.emplace(renderableA, core::TransformComponent{});
    world.transforms.emplace(renderableB, core::TransformComponent{glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(point, core::TransformComponent{});
    world.transforms.emplace(spot, core::TransformComponent{glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.bounds.emplace(renderableA, core::BoundsComponent{render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)}});
    world.bounds.emplace(renderableB, core::BoundsComponent{render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)}});
    world.renderables.emplace(renderableA, core::RenderableComponent{render::MeshHandle{0}, render::RenderLayer::Geometry});
    world.renderables.emplace(renderableB, core::RenderableComponent{render::MeshHandle{1}, render::RenderLayer::Actors});
    world.materials.emplace(renderableA, core::MaterialComponent{material});
    world.materials.emplace(renderableB, core::MaterialComponent{material});
    world.pointLights.emplace(point, core::PointLightComponent{4.0f, glm::vec3(1.0f), 1.0f, 0.0f, false, false, 0.0f, 0.0f});
    world.spotLights.emplace(spot, core::SpotLightComponent{5.0f, glm::vec3(1.0f), 1.0f, glm::vec3(0.0f), 15.0f, 25.0f, 0.0f, false, false, 0.0f, 0.0f});
    world.markTransformsDirty(renderableA);
    world.markTransformsDirty(renderableB);
    world.markTransformsDirty(point);
    world.markTransformsDirty(spot);

    transformSystem.update(world, scheduler);
    lightSystem.update(world, core::TimeContext{1.0f, 0.016f}, scheduler);

    core::FrameSceneData frame;
    extraction.extract(world, selection, frame, scheduler);

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
    core::SelectionModel selection;
    core::AnimationSystem animationSystem;
    core::TransformSystem transformSystem;
    core::RenderExtractionSystem extraction;

    const std::shared_ptr<render::Material> material = makeMaterial();

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
    world.renderables.emplace(sectionEntity, core::RenderableComponent{render::MeshHandle{0}, render::RenderLayer::Actors});
    world.materials.emplace(sectionEntity, core::MaterialComponent{material});
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
    extraction.extract(world, selection, frame, scheduler, false);
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
    extraction.extract(world, selection, frame, scheduler, false);
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
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::RenderExtractionSystem extraction;

    const std::shared_ptr<render::Material> material = makeMaterial();

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
        render::RenderLayer::Actors
    });
    world.materials.emplace(sectionEntity, core::MaterialComponent{material});
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
    extraction.extract(world, selection, frame, scheduler, false);

    assert(frame.renderables.size() == 1u);
    assert(frame.renderables[0].skinned);
    assert(nearlyEqual(frame.renderables[0].worldBounds.min.x, 4.0, 0.0001));
    assert(nearlyEqual(frame.renderables[0].worldBounds.max.x, 8.0, 0.0001));
    assert(nearlyEqual(frame.selection.worldBounds.min.x, 4.0, 0.0001));
    assert(nearlyEqual(frame.selection.worldBounds.max.x, 8.0, 0.0001));
    assert(nearlyEqual(frame.selection.transformMatrix[0][0], 2.0, 0.0001));

    frame.clear();
    selection.set(root);
    extraction.extract(world, selection, frame, scheduler, false);
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
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::RenderExtractionSystem extraction;

    const std::shared_ptr<render::Material> material = makeMaterial();

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
        render::RenderLayer::Actors
    });
    world.materials.emplace(sectionEntity, core::MaterialComponent{material});
    world.skinnedRenderables.emplace(sectionEntity, core::SkinnedRenderableComponent{root, 0, 0, 0});
    world.markTransformsDirty(root);
    world.markTransformsDirty(sectionEntity);

    transformSystem.update(world, scheduler, false);

    core::FrameSceneData frame;
    selection.set(sectionEntity);
    extraction.extract(world, selection, frame, scheduler, false);

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
    extraction.extract(world, selection, frame, scheduler, false);

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
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{1, 1},
        render::MeshHandle{0},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(0.0f), glm::vec3(1.0f)},
        render::Bounds3{glm::vec3(2.0f), glm::vec3(3.0f)}
    ));
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
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{1, 1},
        render::MeshHandle{0},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)}
    ));
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{2, 1},
        render::MeshHandle{1},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(1.5f, -0.25f, -2.0f), glm::vec3(2.0f, 0.25f, -1.5f)}
    ));
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{3, 1},
        render::MeshHandle{2},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        true,
        false
    ));
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{4, 1},
        render::MeshHandle{3},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{},
        false
    ));

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
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{5, 1},
        render::MeshHandle{0},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f, -0.25f, -12.0f), glm::vec3(0.25f, 0.25f, -11.5f)}
    ));

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
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{10, 1},
        render::MeshHandle{0},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)}
    ));
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{11, 1},
        render::MeshHandle{1},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(0.1f, 0.1f, -2.0f), glm::vec3(0.6f, 0.6f, -1.5f)}
    ));
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{12, 1},
        render::MeshHandle{2},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{},
        false
    ));
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{13, 1},
        render::MeshHandle{3},
        render::RenderLayer::Ground,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)}
    ));

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
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{15, 1},
        render::MeshHandle{0},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(-0.25f, -0.25f, -2.0f), glm::vec3(0.25f, 0.25f, -1.5f)}
    ));

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
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{14, 1},
        render::MeshHandle{0},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(-0.25f), glm::vec3(0.25f)},
        render::Bounds3{glm::vec3(2.0f, 2.0f, -2.0f), glm::vec3(2.5f, 2.5f, -1.5f)}
    ));

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
    core::SelectionModel selection;
    core::TransformSystem transformSystem;
    core::LightSystem lightSystem;
    core::RenderExtractionSystem extraction;
    core::FrameSceneData frame;

    const std::shared_ptr<render::Material> material = makeMaterial("Frame Prep");

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
    world.renderables.emplace(renderableA, core::RenderableComponent{render::MeshHandle{0}, render::RenderLayer::Geometry});
    world.renderables.emplace(renderableB, core::RenderableComponent{render::MeshHandle{1}, render::RenderLayer::Actors});
    world.materials.emplace(renderableA, core::MaterialComponent{material});
    world.materials.emplace(renderableB, core::MaterialComponent{material});
    world.visibilities.emplace(renderableA, core::VisibilityComponent{true});
    world.visibilities.emplace(renderableB, core::VisibilityComponent{true});
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
    const core::EntityId lightVolume = addLightVolumeEntity(world, glm::vec3(0.0f), glm::vec3(10.0f));

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
        extraction.extract(world, selection, frame, scheduler);
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
        assert(frame.lightVolumes[0].entity == lightVolume);
        assert(frame.lightVolumes[0].staticLightIndices.size() == 1u);
        assert(frame.lightVolumes[0].movableLightIndices.size() == 1u);
        assert(world.transformCache_[renderableA.index].valid);
        assert(world.transformCache_[renderableB.index].valid);
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

void testPhysicsSphereSphereResolvesOverlap() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler(4u);
    core::PhysicsSystem physics;

    const core::EntityId a = world.createEntity();
    const core::EntityId b = world.createEntity();
    world.transforms.emplace(a, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(b, core::TransformComponent{glm::vec3(0.75f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.sphereColliders.emplace(a, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, false, false});
    world.sphereColliders.emplace(b, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, false, false});
    world.rigidbodies.emplace(a, core::RigidbodyComponent{1.0f, 0.0f, 0.0f, false, false});
    world.rigidbodies.emplace(b, core::RigidbodyComponent{1.0f, 0.0f, 0.0f, false, false});

    physics.update(world, core::TimeContext{0.0f, 0.0f}, scheduler, true);

    const glm::vec3 delta = world.transforms.get(b).position - world.transforms.get(a).position;
    assert(glm::length(delta) >= 1.0f - 0.0001f);
    assert(nearlyEqual((world.transforms.get(a).position.x + world.transforms.get(b).position.x) * 0.5f, 0.375, 0.001));
}

void testPhysicsBoxBoxResolvesOverlap() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler(4u);
    core::PhysicsSystem physics;

    const core::EntityId a = world.createEntity();
    const core::EntityId b = world.createEntity();
    world.transforms.emplace(a, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(b, core::TransformComponent{glm::vec3(0.75f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.boxColliders.emplace(a, core::BoxColliderComponent{glm::vec3(0.0f), glm::vec3(0.5f), false, false});
    world.boxColliders.emplace(b, core::BoxColliderComponent{glm::vec3(0.0f), glm::vec3(0.5f), false, false});
    world.rigidbodies.emplace(a, core::RigidbodyComponent{1.0f, 0.0f, 0.0f, false, false});
    world.rigidbodies.emplace(b, core::RigidbodyComponent{1.0f, 0.0f, 0.0f, false, false});

    physics.update(world, core::TimeContext{0.0f, 0.0f}, scheduler, true);

    const float separation = world.transforms.get(b).position.x - world.transforms.get(a).position.x;
    assert(separation >= 1.0f - 0.0001f);
    assert(nearlyEqual((world.transforms.get(a).position.x + world.transforms.get(b).position.x) * 0.5f, 0.375, 0.001));
}

void testPhysicsRotatedBoxDoesNotCollideUsingInflatedAabbOnly() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler(4u);
    core::PhysicsSystem physics;

    const core::EntityId box = world.createEntity();
    const core::EntityId sphere = world.createEntity();
    world.transforms.emplace(box, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f, 45.0f, 0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(sphere, core::TransformComponent{glm::vec3(0.6f, 0.0f, 0.6f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.boxColliders.emplace(box, core::BoxColliderComponent{glm::vec3(0.0f), glm::vec3(0.5f), false, false});
    world.sphereColliders.emplace(sphere, core::SphereColliderComponent{glm::vec3(0.0f), 0.1f, false, false});
    world.rigidbodies.emplace(box, core::RigidbodyComponent{1.0f, 0.0f, 0.0f, true, false});
    world.rigidbodies.emplace(sphere, core::RigidbodyComponent{1.0f, 0.0f, 0.0f, false, false});

    physics.update(world, core::TimeContext{0.0f, 0.0f}, scheduler, true);

    assert(nearlyEqual(world.transforms.get(box).position.x, 0.0, 0.0001));
    assert(nearlyEqual(world.transforms.get(box).position.z, 0.0, 0.0001));
    assert(nearlyEqual(world.transforms.get(sphere).position.x, 0.6, 0.0001));
    assert(nearlyEqual(world.transforms.get(sphere).position.z, 0.6, 0.0001));
}

void testPhysicsDynamicBodySeparatesAgainstKinematicBody() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler(4u);
    core::PhysicsSystem physics;

    const core::EntityId kinematic = world.createEntity();
    const core::EntityId dynamic = world.createEntity();
    world.transforms.emplace(kinematic, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(dynamic, core::TransformComponent{glm::vec3(0.75f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.sphereColliders.emplace(kinematic, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, false, false});
    world.sphereColliders.emplace(dynamic, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, false, false});
    world.rigidbodies.emplace(kinematic, core::RigidbodyComponent{1.0f, 0.0f, 0.0f, true, false});
    world.rigidbodies.emplace(dynamic, core::RigidbodyComponent{1.0f, 0.0f, 0.0f, false, false, glm::vec3(-1.0f, 0.0f, 0.0f)});

    physics.update(world, core::TimeContext{0.0f, 0.0f}, scheduler, true);

    assert(nearlyEqual(world.transforms.get(kinematic).position.x, 0.0, 0.0001));
    assert(world.transforms.get(dynamic).position.x >= 1.0f - 0.0001f);
    assert(nearlyEqual(world.rigidbodies.get(dynamic).velocity.x, 0.0, 0.0001));
}

void testPhysicsIgnoresCollidersWithoutRigidbodies() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler(4u);
    core::PhysicsSystem physics;

    const core::EntityId colliderOnly = world.createEntity();
    const core::EntityId dynamic = world.createEntity();
    world.transforms.emplace(colliderOnly, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(dynamic, core::TransformComponent{glm::vec3(0.75f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.sphereColliders.emplace(colliderOnly, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, false, false});
    world.sphereColliders.emplace(dynamic, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, false, false});
    world.rigidbodies.emplace(dynamic, core::RigidbodyComponent{1.0f, 0.0f, 0.0f, false, false});

    physics.update(world, core::TimeContext{0.0f, 0.0f}, scheduler, true);

    assert(nearlyEqual(world.transforms.get(dynamic).position.x, 0.75, 0.0001));
}

void testPhysicsTriggerFlagDoesNotDisableCollisionResolution() {
    core::World world;
    core::TaskScheduler scheduler = makeScheduler(4u);
    core::PhysicsSystem physics;

    const core::EntityId a = world.createEntity();
    const core::EntityId b = world.createEntity();
    world.transforms.emplace(a, core::TransformComponent{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.transforms.emplace(b, core::TransformComponent{glm::vec3(0.75f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.sphereColliders.emplace(a, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, true, false});
    world.sphereColliders.emplace(b, core::SphereColliderComponent{glm::vec3(0.0f), 0.5f, true, false});
    world.rigidbodies.emplace(a, core::RigidbodyComponent{1.0f, 0.0f, 0.0f, false, false});
    world.rigidbodies.emplace(b, core::RigidbodyComponent{1.0f, 0.0f, 0.0f, false, false});

    physics.update(world, core::TimeContext{0.0f, 0.0f}, scheduler, true);

    const glm::vec3 delta = world.transforms.get(b).position - world.transforms.get(a).position;
    assert(glm::length(delta) >= 1.0f - 0.0001f);
}

void testActiveLightSelectionDeduplicatesAcrossVolumes() {
    std::vector<core::FrameLight> lights{
        core::FrameLight{core::EntityId{1, 1}, render::LightType::Point},
        core::FrameLight{core::EntityId{2, 1}, render::LightType::Spot},
        core::FrameLight{core::EntityId{3, 1}, render::LightType::Point},
    };
    std::vector<core::FrameLightVolume> volumes{
        core::FrameLightVolume{core::EntityId{10, 1}, {}, {}, {0, 1}, {2}},
        core::FrameLightVolume{core::EntityId{11, 1}, {}, {}, {1}, {2}},
    };

    const render::ActiveLightSelection selection = render::selectActiveLights(lights, volumes);
    assert(selection.indices.size() == 3u);
    assert(selection.pointLightCount == 2);
    assert(selection.spotLightCount == 1);
    assert(selection.indices[0] == 0);
    assert(selection.indices[1] == 1);
    assert(selection.indices[2] == 2);
}

void testSpotShadowRegistrationTracksLinearDepthInputs() {
    render::ShadowSystem shadowSystem;
    shadowSystem.beginFrame();

    const render::ShadowSystem::SpotShadowDesc desc{
        glm::vec3(2.0f, 3.0f, 4.0f),
        glm::normalize(glm::vec3(-1.0f, -0.5f, -2.0f)),
        12.0f,
        25.0f,
        0.0f,
        0.0f,
    };

    const int firstIndex = shadowSystem.registerSpotShadow(desc, glm::mat4(1.0f));
    assert(firstIndex == 0);
    assert(shadowSystem.spotShadowCount() == 1);
    assert(nearlyEqual(shadowSystem.spotShadowPositions()[firstIndex].x, desc.position.x, 0.0001));
    assert(nearlyEqual(shadowSystem.spotShadowPositions()[firstIndex].y, desc.position.y, 0.0001));
    assert(nearlyEqual(shadowSystem.spotShadowPositions()[firstIndex].z, desc.position.z, 0.0001));
    assert(nearlyEqual(shadowSystem.spotShadowFarPlanes()[firstIndex], desc.radius, 0.0001));

    const render::ShadowSystem::SpotShadowDesc clampedDesc{
        glm::vec3(-1.0f, 0.5f, 2.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        0.05f,
        18.0f,
        0.0f,
        0.0f,
    };

    const int secondIndex = shadowSystem.registerSpotShadow(clampedDesc, glm::mat4(1.0f));
    assert(secondIndex == 1);
    assert(nearlyEqual(shadowSystem.spotShadowFarPlanes()[secondIndex], 0.2f, 0.0001));
}

void testPickingSystemCanIgnoreLights() {
    core::FrameSceneData frame;
    frame.renderables.push_back(makeFrameRenderable(
        core::EntityId{1, 1},
        render::MeshHandle{0},
        render::RenderLayer::Geometry,
        render::Bounds3{glm::vec3(0.0f)},
        render::Bounds3{glm::vec3(-0.25f, -0.25f, -6.0f), glm::vec3(0.25f, 0.25f, -4.0f)}
    ));
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

void testNavigationAsyncRequestPublishesPartialPathBeforeFinalApply() {
    core::TaskScheduler scheduler = makeScheduler();
    core::NavigationSystem navigation;
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = makeLinearNavPolygons(24u);
    assert(navigation.rebuildRuntime(runtime));

    core::World world;
    const core::EntityId agentEntity = world.createEntity();
    world.transforms.emplace(agentEntity, core::TransformComponent{glm::vec3(0.25f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)});
    world.navAgents.emplace(agentEntity, core::NavAgentComponent{});

    const glm::vec3 requestedDestination(23.75f, 0.0f, 0.0f);
    assert(navigation.requestAgentDestination(world, runtime, scheduler, agentEntity, requestedDestination));

    bool observedPartialPath = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        navigation.applyCompletedPathRequests(world, runtime);
        const auto counters = navigation.profilingCounters();
        const std::int64_t pending = requireCounterValue(counters, "Pending Path Requests", "Navigation");
        const core::NavAgentComponent& agent = world.navAgents.get(agentEntity);
        if (pending == 1 &&
            agent.destination.has_value() &&
            nearlyEqualVec3(*agent.destination, requestedDestination) &&
            !agent.pathCorners.empty() &&
            nearlyEqualVec3(agent.pathCorners.back(), requestedDestination)) {
            observedPartialPath = true;
            break;
        }
        if (pending == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    assert(observedPartialPath);
    waitForNavigationRequestsToDrain(navigation, world, runtime);
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
    if (envFlagEnabled("ALKANZAR_WITNESS_RANDOM_NAV_HANG")) {
        witnessRandomBoxClearanceNavmeshHang();
        return EXIT_SUCCESS;
    }
    testEntityPoolReuse();
    testComponentStoreDenseRemove();
    testEventBusOrderingAndUnsubscribe();
    testCommandHistoryUndoRedoAndMerge();
    testEditorSessionWindowVisibilityHelpers();
    testEditorSessionImGuiSettingsRoundTrip();
    testSelectionModelTracksComponentFocus();
    testNavigationAssetRoundTrip();
    testNavigationBakeBuildsMultiLevelPolygonsAndBlocksOnlyOverlappingLayers();
    testNavigationBakeFiltersRuntimeCellsBelowConfiguredArea();
    testNavigationGenerationUsesUnionOfObjectColliderShapes();
    testNavigationDefaultHitboxPreservesConcaveShapeUnion();
    testNavigationGenerationMinimizesRectangularObstacleDecomposition();
    testNavigationGenerationHandlesFantasyHouseGeometryWithoutPathologicalMerge();
    testNavigationPathfindingUsesIntervalSearchAndExplicitLinks();
    testNavigationOverlapCorridorConstraint();
    testNavigationConcavePolygonStaysInsideWalkableSurface();
    testNavigationOverlapPrefersShortestStraightCorridor();
    testNavigationPathfindingChoosesShortestGeometricCorridor();
    testNavigationPathfindingKeepsShortestCorridorPastSixtyFourBoundaryNodes();
    testNavigationOverlapCandidateSelectionUsesMultiplyCoveredStartCell();
    testNavigationRejectsSelfIntersectingPolygon();
    testNavigationHitTestFindsProjectedNavPolygon();
    testNavigationAgentMovementRotatesAndRequestsWalkThenIdle();
    testNavigationAgentClearanceRemainsOptIn();
    testNavigationAgentSphereClearancePullsDestinationAwayFromWalls();
    testNavigationAgentSphereClearanceRejectsTooNarrowCorridor();
    testNavigationAgentBoxClearanceUsesLateralFootprintAndKeepsDirectPath();
    testNavigationRotatedBoxClearancePreservesExplicitLinkTransitions();
    testNavigationClearanceResolvesTowardApproachInsteadOfCornerVertex();
    testNavigationOverlapLShapeUsesShortcutThroughOverlapRegion();
    testNavigationOverlapTShapeRoutesThroughWideOverlap();
    testNavigationOverlapDiagonalCrossing();
    testNavigationOverlapFullContainmentUsesStraightPath();
    testNavigationOverlapThreePolygonChain();
    testNavigationDefaultSceneNavmeshBoxClearanceOverlap();
    testNavigationClearancePathfindingAvoidsIntervalStateExplosion();
    testTaskSchedulerParallelForCoversFullRange();
    testTaskSchedulerWaitCompletesScheduledGroup();
    testTaskSchedulerAsyncHandleDeliversResult();
    testTaskSchedulerWaitRethrowsTaskFailures();
    testTaskSchedulerRepeatedPhaseWaitsComplete();
    testTransformHierarchyAndBounds();
    testTransformSystemProcessesMultipleRoots();
    testTransformMathBuildsRotatedBoxesWithoutInflatingLocalExtents();
    testLightVolumeAssignment();
    testLightVolumeAssignmentIsStableAcrossLightTypes();
    testMaterialComponentSharingExtraction();
    testRenderExtractionDefaultsMissingVisibilityToHidden();
    testRenderExtractionTracksFocusedComponentSelection();
    testRenderExtractionEmitsVisibleColliderDebugOnly();
    testRenderExtractionUsesOrientedBoxMatricesForRotatedColliders();
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
    testPhysicsSphereSphereResolvesOverlap();
    testPhysicsBoxBoxResolvesOverlap();
    testPhysicsRotatedBoxDoesNotCollideUsingInflatedAabbOnly();
    testPhysicsDynamicBodySeparatesAgainstKinematicBody();
    testPhysicsIgnoresCollidersWithoutRigidbodies();
    testPhysicsTriggerFlagDoesNotDisableCollisionResolution();
    testActiveLightSelectionDeduplicatesAcrossVolumes();
    testSpotShadowRegistrationTracksLinearDepthInputs();
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
    testNavigationAsyncRequestPublishesPartialPathBeforeFinalApply();
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
