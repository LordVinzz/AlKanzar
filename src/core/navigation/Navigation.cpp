#include "Navigation.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>

#include <SDL.h>
#include <glm/geometric.hpp>

#include "core/systems/PickingSystem.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace core {

namespace {

constexpr float kHorizontalNormalMinDot = 0.9848077f;
constexpr float kLayerGroupingEpsilon = 0.10f;
constexpr float kPolygonEpsilon = 1.0e-4f;
constexpr float kPlaneEpsilon = 1.0e-5f;
constexpr int kNavAssetVersion = 1;
const glm::vec4 kWalkableOverlayColor(0.0f, 1.0f, 0.0f, 0.5f);

struct QuantizedVec2 {
    long long x{0};
    long long y{0};

    friend bool operator==(QuantizedVec2 lhs, QuantizedVec2 rhs) = default;
    friend bool operator<(QuantizedVec2 lhs, QuantizedVec2 rhs) {
        if (lhs.x != rhs.x) {
            return lhs.x < rhs.x;
        }
        return lhs.y < rhs.y;
    }
};

struct QuantizedVec2Hash {
    std::size_t operator()(const QuantizedVec2& value) const noexcept {
        return static_cast<std::size_t>(value.x * 73856093ull) ^ static_cast<std::size_t>(value.y * 19349663ull);
    }
};

struct QuantizedEdge {
    QuantizedVec2 a{};
    QuantizedVec2 b{};

    friend bool operator==(const QuantizedEdge&, const QuantizedEdge&) = default;
};

struct QuantizedEdgeHash {
    std::size_t operator()(const QuantizedEdge& edge) const noexcept {
        QuantizedVec2Hash hash{};
        return hash(edge.a) ^ (hash(edge.b) << 1u);
    }
};

struct WalkableTriangle {
    float elevationY{0.0f};
    glm::vec2 a{0.0f};
    glm::vec2 b{0.0f};
    glm::vec2 c{0.0f};
};

struct BlockingFootprint {
    float minY{0.0f};
    float maxY{0.0f};
    glm::vec2 minXZ{0.0f};
    glm::vec2 maxXZ{0.0f};
};

struct LayerBuildData {
    float elevationY{0.0f};
    std::vector<WalkableTriangle> triangles{};
    std::vector<BlockingFootprint> blockers{};
};

struct AuthoredBakePolygon {
    std::size_t assetIndex{0u};
    float elevationY{0.0f};
    std::vector<glm::vec2> verticesXZ{};
};

struct BakedTriangle {
    std::size_t authoredPolygonIndex{0u};
    float elevationY{0.0f};
    std::array<glm::vec2, 3u> verticesXZ{};
};

struct BakeLayerData {
    float elevationY{0.0f};
    std::vector<AuthoredBakePolygon> polygons{};
    std::vector<BakedTriangle> triangles{};
};

struct ParentPathData {
    std::unordered_map<EntityId, std::vector<EntityId>> childrenByParent{};
    std::unordered_map<EntityId, std::string> pathByEntity{};
};

struct SharedPortalResult {
    glm::vec2 a{0.0f};
    glm::vec2 b{0.0f};
};

float cross2(const glm::vec2& lhs, const glm::vec2& rhs) {
    return lhs.x * rhs.y - lhs.y * rhs.x;
}

float triArea2(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    return cross2(b - a, c - a);
}

bool nearlyEqual(float lhs, float rhs, float epsilon = kPolygonEpsilon) {
    return std::abs(lhs - rhs) <= epsilon;
}

bool nearlyEqualVec2(const glm::vec2& lhs, const glm::vec2& rhs, float epsilon = kPolygonEpsilon) {
    return glm::length(lhs - rhs) <= epsilon;
}

bool nearlyEqualVec3(const glm::vec3& lhs, const glm::vec3& rhs, float epsilon = kPolygonEpsilon) {
    return glm::length(lhs - rhs) <= epsilon;
}

QuantizedVec2 quantizeVec2(const glm::vec2& value) {
    return QuantizedVec2{
        static_cast<long long>(std::llround(static_cast<double>(value.x) * 10000.0)),
        static_cast<long long>(std::llround(static_cast<double>(value.y) * 10000.0))
    };
}

std::string lowercaseCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string entityName(const World& world, EntityId entity) {
    if (const NameComponent* name = world.names.tryGet(entity)) {
        return name->value;
    }
    return "Entity " + std::to_string(entity.index);
}

ParentPathData buildStableIdPaths(const World& world) {
    ParentPathData data{};
    std::unordered_set<EntityId> entities{};
    for (EntityId entity : world.names.entities()) {
        entities.insert(entity);
    }
    for (EntityId entity : world.parents.entities()) {
        entities.insert(entity);
        const ParentComponent& parent = world.parents.get(entity);
        if (parent.parent.valid() && world.isAlive(parent.parent)) {
            entities.insert(parent.parent);
            data.childrenByParent[parent.parent].push_back(entity);
        }
    }
    for (EntityId entity : world.renderables.entities()) {
        entities.insert(entity);
    }
    for (EntityId entity : world.transforms.entities()) {
        entities.insert(entity);
    }

    std::vector<EntityId> roots{};
    roots.reserve(entities.size());
    for (EntityId entity : entities) {
        const ParentComponent* parent = world.parents.tryGet(entity);
        if (parent == nullptr || !parent->parent.valid() || !world.isAlive(parent->parent)) {
            roots.push_back(entity);
        }
    }

    const auto compareByLabel = [&world](EntityId lhs, EntityId rhs) {
        const std::string lhsLabel = entityName(world, lhs);
        const std::string rhsLabel = entityName(world, rhs);
        if (lhsLabel != rhsLabel) {
            return lhsLabel < rhsLabel;
        }
        return lhs.index < rhs.index;
    };

    std::sort(roots.begin(), roots.end(), compareByLabel);
    for (auto& [parent, children] : data.childrenByParent) {
        std::sort(children.begin(), children.end(), compareByLabel);
    }

    const auto visit = [&](const auto& self, EntityId entity, const std::string& parentPath) -> void {
        std::vector<EntityId> siblings{};
        const ParentComponent* parent = world.parents.tryGet(entity);
        if (parent != nullptr && parent->parent.valid() && world.isAlive(parent->parent)) {
            auto it = data.childrenByParent.find(parent->parent);
            if (it != data.childrenByParent.end()) {
                siblings = it->second;
            }
        } else {
            siblings = roots;
        }

        const std::string label = entityName(world, entity);
        int ordinal = 0;
        int matchingCount = 0;
        for (EntityId sibling : siblings) {
            if (entityName(world, sibling) != label) {
                continue;
            }
            if (sibling == entity) {
                ordinal = matchingCount;
            }
            ++matchingCount;
        }

        std::string segment = label;
        if (matchingCount > 1) {
            segment += "#" + std::to_string(ordinal);
        }
        const std::string path = parentPath.empty() ? segment : parentPath + "/" + segment;
        data.pathByEntity[entity] = path;

        auto it = data.childrenByParent.find(entity);
        if (it == data.childrenByParent.end()) {
            return;
        }
        for (EntityId child : it->second) {
            self(self, child, path);
        }
    };

    for (EntityId root : roots) {
        visit(visit, root, std::string{});
    }

    return data;
}

NavSourceTag defaultTagForLayer(render::RenderLayer layer) {
    switch (layer) {
        case render::RenderLayer::Ground:
            return NavSourceTag::Walkable;
        case render::RenderLayer::Actors:
            return NavSourceTag::Ignored;
        case render::RenderLayer::Geometry:
        default:
            return NavSourceTag::Blocking;
    }
}

bool pointInPolygonXZ(const glm::vec2& point, const std::vector<glm::vec2>& polygon) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1u; i < polygon.size(); j = i++) {
        const glm::vec2& a = polygon[i];
        const glm::vec2& b = polygon[j];
        const bool intersect = ((a.y > point.y) != (b.y > point.y)) &&
            (point.x < ((b.x - a.x) * (point.y - a.y) / ((b.y - a.y) + kPlaneEpsilon) + a.x));
        if (intersect) {
            inside = !inside;
        }
    }
    return inside;
}

bool pointOnSegmentXZ(const glm::vec2& point, const glm::vec2& a, const glm::vec2& b) {
    const glm::vec2 ab = b - a;
    const glm::vec2 ap = point - a;
    if (std::abs(cross2(ab, ap)) > kPolygonEpsilon) {
        return false;
    }
    return glm::dot(point - a, point - b) <= kPolygonEpsilon;
}

bool pointInOrOnPolygonXZ(const glm::vec2& point, const std::vector<glm::vec2>& polygon) {
    if (polygon.size() < 3u) {
        return false;
    }
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        if (pointOnSegmentXZ(point, polygon[index], polygon[(index + 1u) % polygon.size()])) {
            return true;
        }
    }
    return pointInPolygonXZ(point, polygon);
}

bool pointInTriangleXZ(const glm::vec2& point, const WalkableTriangle& tri) {
    const float w0 = triArea2(point, tri.b, tri.c);
    const float w1 = triArea2(point, tri.c, tri.a);
    const float w2 = triArea2(point, tri.a, tri.b);
    const bool hasNegative = (w0 < -kPolygonEpsilon) || (w1 < -kPolygonEpsilon) || (w2 < -kPolygonEpsilon);
    const bool hasPositive = (w0 > kPolygonEpsilon) || (w1 > kPolygonEpsilon) || (w2 > kPolygonEpsilon);
    return !(hasNegative && hasPositive);
}

float polygonSignedArea(const std::vector<glm::vec2>& vertices) {
    float twiceArea = 0.0f;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const glm::vec2& a = vertices[index];
        const glm::vec2& b = vertices[(index + 1u) % vertices.size()];
        twiceArea += cross2(a, b);
    }
    return twiceArea * 0.5f;
}

bool polygonHasArea(const std::vector<glm::vec2>& vertices) {
    return vertices.size() >= 3u && std::abs(polygonSignedArea(vertices)) > kPolygonEpsilon;
}

void removeConsecutiveDuplicateVertices(std::vector<glm::vec2>& vertices) {
    std::vector<glm::vec2> filtered{};
    filtered.reserve(vertices.size());
    for (const glm::vec2& vertex : vertices) {
        if (!filtered.empty() && nearlyEqualVec2(filtered.back(), vertex)) {
            continue;
        }
        filtered.push_back(vertex);
    }
    if (filtered.size() >= 2u && nearlyEqualVec2(filtered.front(), filtered.back())) {
        filtered.pop_back();
    }
    vertices = std::move(filtered);
}

void removeColinearVertices(std::vector<glm::vec2>& vertices) {
    if (vertices.size() < 3u) {
        return;
    }

    bool changed = true;
    while (changed && vertices.size() >= 3u) {
        changed = false;
        for (std::size_t index = 0; index < vertices.size(); ++index) {
            const std::size_t prevIndex = (index + vertices.size() - 1u) % vertices.size();
            const std::size_t nextIndex = (index + 1u) % vertices.size();
            const glm::vec2& prev = vertices[prevIndex];
            const glm::vec2& current = vertices[index];
            const glm::vec2& next = vertices[nextIndex];
            if (pointOnSegmentXZ(current, prev, next)) {
                vertices.erase(vertices.begin() + static_cast<std::ptrdiff_t>(index));
                changed = true;
                break;
            }
        }
    }
}

std::vector<glm::vec2> normalizePolygonVertices(const std::vector<glm::vec2>& vertices) {
    std::vector<glm::vec2> normalized = vertices;
    removeConsecutiveDuplicateVertices(normalized);
    removeColinearVertices(normalized);
    if (normalized.size() >= 3u && polygonSignedArea(normalized) < 0.0f) {
        std::reverse(normalized.begin(), normalized.end());
    }
    return normalized;
}

glm::vec2 polygonCentroidXZ(const std::vector<glm::vec2>& vertices) {
    glm::vec2 centroid(0.0f);
    if (vertices.empty()) {
        return centroid;
    }

    float twiceArea = 0.0f;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const glm::vec2& a = vertices[index];
        const glm::vec2& b = vertices[(index + 1u) % vertices.size()];
        const float cross = cross2(a, b);
        twiceArea += cross;
        centroid += (a + b) * cross;
    }
    if (std::abs(twiceArea) <= kPolygonEpsilon) {
        for (const glm::vec2& vertex : vertices) {
            centroid += vertex;
        }
        return centroid / static_cast<float>(vertices.size());
    }
    return centroid / (3.0f * twiceArea);
}

bool polygonValid(const NavPolygon& polygon) {
    return polygon.id >= 0 && polygon.verticesXZ.size() >= 3u;
}

int orientationSign(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    const float area = triArea2(a, b, c);
    if (area > kPolygonEpsilon) {
        return 1;
    }
    if (area < -kPolygonEpsilon) {
        return -1;
    }
    return 0;
}

bool segmentsIntersectInclusive(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec2& d) {
    const int o1 = orientationSign(a, b, c);
    const int o2 = orientationSign(a, b, d);
    const int o3 = orientationSign(c, d, a);
    const int o4 = orientationSign(c, d, b);

    if (o1 != o2 && o3 != o4) {
        return true;
    }
    if (o1 == 0 && pointOnSegmentXZ(c, a, b)) {
        return true;
    }
    if (o2 == 0 && pointOnSegmentXZ(d, a, b)) {
        return true;
    }
    if (o3 == 0 && pointOnSegmentXZ(a, c, d)) {
        return true;
    }
    if (o4 == 0 && pointOnSegmentXZ(b, c, d)) {
        return true;
    }
    return false;
}

bool polygonIsSimple(const std::vector<glm::vec2>& vertices) {
    if (vertices.size() < 3u) {
        return false;
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const glm::vec2& a = vertices[i];
        const glm::vec2& b = vertices[(i + 1u) % vertices.size()];
        if (nearlyEqualVec2(a, b)) {
            return false;
        }
        for (std::size_t j = i + 1u; j < vertices.size(); ++j) {
            const std::size_t iNext = (i + 1u) % vertices.size();
            const std::size_t jNext = (j + 1u) % vertices.size();
            if (i == j || i == jNext || iNext == j) {
                continue;
            }
            if (segmentsIntersectInclusive(a, b, vertices[j], vertices[jNext])) {
                return false;
            }
        }
    }
    return true;
}

std::vector<std::array<glm::vec2, 3u>> triangulateSimplePolygon(const std::vector<glm::vec2>& polygon) {
    std::vector<std::array<glm::vec2, 3u>> triangles{};
    if (polygon.size() < 3u) {
        return triangles;
    }
    if (polygon.size() == 3u) {
        triangles.push_back({polygon[0], polygon[1], polygon[2]});
        return triangles;
    }

    std::vector<std::size_t> remaining{};
    remaining.reserve(polygon.size());
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        remaining.push_back(index);
    }

    std::size_t guard = 0u;
    while (remaining.size() > 3u && guard < polygon.size() * polygon.size()) {
        bool clippedEar = false;
        for (std::size_t localIndex = 0; localIndex < remaining.size(); ++localIndex) {
            const std::size_t prevLocal = (localIndex + remaining.size() - 1u) % remaining.size();
            const std::size_t nextLocal = (localIndex + 1u) % remaining.size();
            const glm::vec2& prev = polygon[remaining[prevLocal]];
            const glm::vec2& current = polygon[remaining[localIndex]];
            const glm::vec2& next = polygon[remaining[nextLocal]];
            if (triArea2(prev, current, next) <= kPolygonEpsilon) {
                continue;
            }

            WalkableTriangle ear{};
            ear.a = prev;
            ear.b = current;
            ear.c = next;

            bool containsOtherVertex = false;
            for (std::size_t candidateIndex = 0; candidateIndex < remaining.size(); ++candidateIndex) {
                if (candidateIndex == prevLocal || candidateIndex == localIndex || candidateIndex == nextLocal) {
                    continue;
                }
                if (pointInTriangleXZ(polygon[remaining[candidateIndex]], ear)) {
                    containsOtherVertex = true;
                    break;
                }
            }
            if (containsOtherVertex) {
                continue;
            }

            triangles.push_back({prev, current, next});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(localIndex));
            clippedEar = true;
            break;
        }
        if (!clippedEar) {
            triangles.clear();
            return triangles;
        }
        ++guard;
    }

    if (remaining.size() == 3u) {
        triangles.push_back({
            polygon[remaining[0]],
            polygon[remaining[1]],
            polygon[remaining[2]]
        });
    }
    return triangles;
}

std::vector<glm::vec2> polygonVerticesFromRect(const glm::vec2& minPoint, const glm::vec2& maxPoint) {
    return {
        glm::vec2(minPoint.x, minPoint.y),
        glm::vec2(maxPoint.x, minPoint.y),
        glm::vec2(maxPoint.x, maxPoint.y),
        glm::vec2(minPoint.x, maxPoint.y),
    };
}

bool blockerOverlapsLayer(const BlockingFootprint& blocker, float elevationY) {
    return elevationY >= blocker.minY - kLayerGroupingEpsilon &&
        elevationY <= blocker.maxY + kLayerGroupingEpsilon;
}

bool pointInsideBlocker(const glm::vec2& point, const BlockingFootprint& blocker) {
    return point.x >= blocker.minXZ.x - kPolygonEpsilon &&
        point.x <= blocker.maxXZ.x + kPolygonEpsilon &&
        point.y >= blocker.minXZ.y - kPolygonEpsilon &&
        point.y <= blocker.maxXZ.y + kPolygonEpsilon;
}

bool pointInsideLayerWalkables(const glm::vec2& point, const LayerBuildData& layer) {
    for (const WalkableTriangle& triangle : layer.triangles) {
        if (pointInTriangleXZ(point, triangle)) {
            return true;
        }
    }
    return false;
}

std::vector<NavPolygon> buildPolygonsForLayer(const LayerBuildData& layer, int& nextPolygonId) {
    std::set<float> splitX{};
    std::set<float> splitZ{};
    for (const WalkableTriangle& triangle : layer.triangles) {
        splitX.insert(triangle.a.x);
        splitX.insert(triangle.b.x);
        splitX.insert(triangle.c.x);
        splitZ.insert(triangle.a.y);
        splitZ.insert(triangle.b.y);
        splitZ.insert(triangle.c.y);
    }
    for (const BlockingFootprint& blocker : layer.blockers) {
        if (!blockerOverlapsLayer(blocker, layer.elevationY)) {
            continue;
        }
        splitX.insert(blocker.minXZ.x);
        splitX.insert(blocker.maxXZ.x);
        splitZ.insert(blocker.minXZ.y);
        splitZ.insert(blocker.maxXZ.y);
    }

    std::vector<float> xs(splitX.begin(), splitX.end());
    std::vector<float> zs(splitZ.begin(), splitZ.end());
    if (xs.size() < 2u || zs.size() < 2u) {
        return {};
    }

    struct Cell {
        int x0{0};
        int x1{0};
        int z0{0};
        int z1{0};
    };

    std::vector<Cell> cells{};
    for (std::size_t xi = 0; xi + 1u < xs.size(); ++xi) {
        for (std::size_t zi = 0; zi + 1u < zs.size(); ++zi) {
            const float minX = xs[xi];
            const float maxX = xs[xi + 1u];
            const float minZ = zs[zi];
            const float maxZ = zs[zi + 1u];
            if (maxX - minX <= kPolygonEpsilon || maxZ - minZ <= kPolygonEpsilon) {
                continue;
            }
            const glm::vec2 center((minX + maxX) * 0.5f, (minZ + maxZ) * 0.5f);
            if (!pointInsideLayerWalkables(center, layer)) {
                continue;
            }
            bool blocked = false;
            for (const BlockingFootprint& blocker : layer.blockers) {
                if (blockerOverlapsLayer(blocker, layer.elevationY) && pointInsideBlocker(center, blocker)) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) {
                continue;
            }
            cells.push_back(Cell{
                static_cast<int>(xi),
                static_cast<int>(xi + 1u),
                static_cast<int>(zi),
                static_cast<int>(zi + 1u)
            });
        }
    }

    std::sort(cells.begin(), cells.end(), [](const Cell& lhs, const Cell& rhs) {
        if (lhs.z0 != rhs.z0) {
            return lhs.z0 < rhs.z0;
        }
        if (lhs.z1 != rhs.z1) {
            return lhs.z1 < rhs.z1;
        }
        return lhs.x0 < rhs.x0;
    });

    std::vector<Cell> horizontalMerges{};
    for (const Cell& cell : cells) {
        if (!horizontalMerges.empty()) {
            Cell& tail = horizontalMerges.back();
            if (tail.z0 == cell.z0 && tail.z1 == cell.z1 && tail.x1 == cell.x0) {
                tail.x1 = cell.x1;
                continue;
            }
        }
        horizontalMerges.push_back(cell);
    }

    std::vector<NavPolygon> polygons{};
    std::vector<std::uint8_t> consumed(horizontalMerges.size(), 0u);
    for (std::size_t index = 0; index < horizontalMerges.size(); ++index) {
        if (consumed[index] != 0u) {
            continue;
        }
        Cell merged = horizontalMerges[index];
        consumed[index] = 1u;
        bool extended = true;
        while (extended) {
            extended = false;
            for (std::size_t candidateIndex = index + 1u; candidateIndex < horizontalMerges.size(); ++candidateIndex) {
                if (consumed[candidateIndex] != 0u) {
                    continue;
                }
                const Cell& candidate = horizontalMerges[candidateIndex];
                if (candidate.x0 == merged.x0 && candidate.x1 == merged.x1 && candidate.z0 == merged.z1) {
                    merged.z1 = candidate.z1;
                    consumed[candidateIndex] = 1u;
                    extended = true;
                }
            }
        }

        polygons.push_back(NavPolygon{
            nextPolygonId++,
            layer.elevationY,
            polygonVerticesFromRect(
                glm::vec2(xs[static_cast<std::size_t>(merged.x0)], zs[static_cast<std::size_t>(merged.z0)]),
                glm::vec2(xs[static_cast<std::size_t>(merged.x1)], zs[static_cast<std::size_t>(merged.z1)])
            )
        });
    }

    return polygons;
}

glm::vec2 transformPointXZ(const glm::mat4& matrix, const glm::vec3& point) {
    const glm::vec3 transformed = glm::vec3(matrix * glm::vec4(point, 1.0f));
    return glm::vec2(transformed.x, transformed.z);
}

glm::vec3 transformPoint3(const glm::mat4& matrix, const glm::vec3& point) {
    return glm::vec3(matrix * glm::vec4(point, 1.0f));
}

std::optional<BlockingFootprint> computeFootprint(const render::Mesh& mesh, const glm::mat4& worldMatrix) {
    bool hasBounds = false;
    BlockingFootprint footprint{};
    for (const glm::vec3& vertex : mesh.positions) {
        const glm::vec3 transformed = transformPoint3(worldMatrix, vertex);
        if (!hasBounds) {
            footprint.minY = transformed.y;
            footprint.maxY = transformed.y;
            footprint.minXZ = glm::vec2(transformed.x, transformed.z);
            footprint.maxXZ = footprint.minXZ;
            hasBounds = true;
            continue;
        }
        footprint.minY = std::min(footprint.minY, transformed.y);
        footprint.maxY = std::max(footprint.maxY, transformed.y);
        footprint.minXZ = glm::min(footprint.minXZ, glm::vec2(transformed.x, transformed.z));
        footprint.maxXZ = glm::max(footprint.maxXZ, glm::vec2(transformed.x, transformed.z));
    }
    if (!hasBounds) {
        return std::nullopt;
    }
    return footprint;
}

void addTriangleToLayer(
    std::vector<LayerBuildData>& layers,
    float elevationY,
    const WalkableTriangle& triangle
) {
    for (LayerBuildData& layer : layers) {
        if (std::abs(layer.elevationY - elevationY) <= kLayerGroupingEpsilon) {
            layer.triangles.push_back(triangle);
            return;
        }
    }
    layers.push_back(LayerBuildData{elevationY, {triangle}, {}});
}

std::optional<SharedPortalResult> sharedPortal(const NavPolygon& lhs, const NavPolygon& rhs) {
    auto appendUnique = [](std::vector<glm::vec2>& points, const glm::vec2& point) {
        for (const glm::vec2& existing : points) {
            if (nearlyEqualVec2(existing, point)) {
                return;
            }
        }
        points.push_back(point);
    };

    auto appendSegmentIntersections = [&](std::vector<glm::vec2>& points,
                                          const glm::vec2& a,
                                          const glm::vec2& b,
                                          const glm::vec2& c,
                                          const glm::vec2& d) {
        const glm::vec2 r = b - a;
        const glm::vec2 s = d - c;
        const float rxs = cross2(r, s);
        const float qpxr = cross2(c - a, r);

        if (std::abs(rxs) <= kPolygonEpsilon && std::abs(qpxr) <= kPolygonEpsilon) {
            for (const glm::vec2& candidate : {a, b, c, d}) {
                if (pointOnSegmentXZ(candidate, a, b) && pointOnSegmentXZ(candidate, c, d)) {
                    appendUnique(points, candidate);
                }
            }
            return;
        }
        if (std::abs(rxs) <= kPolygonEpsilon) {
            return;
        }

        const float t = cross2(c - a, s) / rxs;
        const float u = cross2(c - a, r) / rxs;
        if (t < -kPolygonEpsilon || t > 1.0f + kPolygonEpsilon || u < -kPolygonEpsilon || u > 1.0f + kPolygonEpsilon) {
            return;
        }
        appendUnique(points, a + r * t);
    };

    std::vector<glm::vec2> shared{};
    shared.reserve(lhs.verticesXZ.size() + rhs.verticesXZ.size());
    for (const glm::vec2& lhsVertex : lhs.verticesXZ) {
        if (pointInOrOnPolygonXZ(lhsVertex, rhs.verticesXZ)) {
            appendUnique(shared, lhsVertex);
        }
    }
    for (const glm::vec2& rhsVertex : rhs.verticesXZ) {
        if (pointInOrOnPolygonXZ(rhsVertex, lhs.verticesXZ)) {
            appendUnique(shared, rhsVertex);
        }
    }
    for (std::size_t lhsIndex = 0; lhsIndex < lhs.verticesXZ.size(); ++lhsIndex) {
        const glm::vec2& lhsA = lhs.verticesXZ[lhsIndex];
        const glm::vec2& lhsB = lhs.verticesXZ[(lhsIndex + 1u) % lhs.verticesXZ.size()];
        for (std::size_t rhsIndex = 0; rhsIndex < rhs.verticesXZ.size(); ++rhsIndex) {
            const glm::vec2& rhsA = rhs.verticesXZ[rhsIndex];
            const glm::vec2& rhsB = rhs.verticesXZ[(rhsIndex + 1u) % rhs.verticesXZ.size()];
            appendSegmentIntersections(shared, lhsA, lhsB, rhsA, rhsB);
        }
    }
    if (shared.empty()) {
        return std::nullopt;
    }
    if (shared.size() == 1u) {
        return std::nullopt;
    }

    const glm::vec2 origin = shared.front();
    std::optional<glm::vec2> axisPoint{};
    bool allColinear = true;
    for (std::size_t index = 1; index < shared.size(); ++index) {
        if (nearlyEqualVec2(origin, shared[index])) {
            continue;
        }
        axisPoint = shared[index];
        break;
    }
    if (axisPoint.has_value()) {
        const glm::vec2 axis = *axisPoint - origin;
        for (std::size_t index = 1; index < shared.size(); ++index) {
            if (std::abs(cross2(axis, shared[index] - origin)) > kPolygonEpsilon) {
                allColinear = false;
                break;
            }
        }
    }

    if (!allColinear) {
        return std::nullopt;
    }

    glm::vec2 bestA = shared[0];
    glm::vec2 bestB = shared[1];
    float bestDistance = glm::dot(bestB - bestA, bestB - bestA);
    for (std::size_t i = 0; i < shared.size(); ++i) {
        for (std::size_t j = i + 1u; j < shared.size(); ++j) {
            const float distance = glm::dot(shared[j] - shared[i], shared[j] - shared[i]);
            if (distance > bestDistance) {
                bestDistance = distance;
                bestA = shared[i];
                bestB = shared[j];
            }
        }
    }
    if (nearlyEqualVec2(bestA, bestB)) {
        return std::nullopt;
    }
    return SharedPortalResult{bestA, bestB};
}

std::optional<SharedPortalResult> sharedPortal(const NavRuntimeCell& lhs, const NavRuntimeCell& rhs) {
    const NavPolygon lhsPolygon{-1, lhs.elevationY, lhs.verticesXZ};
    const NavPolygon rhsPolygon{-1, rhs.elevationY, rhs.verticesXZ};
    return sharedPortal(lhsPolygon, rhsPolygon);
}

glm::vec2 orientPortalLeft(
    const glm::vec2& currentCenter,
    const glm::vec2& nextCenter,
    const glm::vec2& a,
    const glm::vec2& b
) {
    const glm::vec2 direction = nextCenter - currentCenter;
    const float side = cross2(direction, b - a);
    return side >= 0.0f ? b : a;
}

glm::vec2 orientPortalRight(
    const glm::vec2& currentCenter,
    const glm::vec2& nextCenter,
    const glm::vec2& a,
    const glm::vec2& b
) {
    const glm::vec2 direction = nextCenter - currentCenter;
    const float side = cross2(direction, b - a);
    return side >= 0.0f ? a : b;
}

std::vector<glm::vec2> stringPull(
    const glm::vec2& start,
    const std::vector<std::pair<glm::vec2, glm::vec2>>& portals,
    const glm::vec2& end
) {
    struct Portal {
        glm::vec2 left{0.0f};
        glm::vec2 right{0.0f};
    };

    std::vector<Portal> funnelPortals{};
    funnelPortals.reserve(portals.size() + 2u);
    funnelPortals.push_back(Portal{start, start});
    for (const auto& portal : portals) {
        funnelPortals.push_back(Portal{portal.first, portal.second});
    }
    funnelPortals.push_back(Portal{end, end});

    std::vector<glm::vec2> result{};
    glm::vec2 portalApex = funnelPortals[0].left;
    glm::vec2 portalLeft = funnelPortals[0].left;
    glm::vec2 portalRight = funnelPortals[0].right;
    int apexIndex = 0;
    int leftIndex = 0;
    int rightIndex = 0;

    result.push_back(portalApex);
    for (int index = 1; index < static_cast<int>(funnelPortals.size()); ++index) {
        const glm::vec2 left = funnelPortals[static_cast<std::size_t>(index)].left;
        const glm::vec2 right = funnelPortals[static_cast<std::size_t>(index)].right;

        if (triArea2(portalApex, portalRight, right) <= 0.0f) {
            if (nearlyEqualVec2(portalApex, portalRight) || triArea2(portalApex, portalLeft, right) > 0.0f) {
                portalRight = right;
                rightIndex = index;
            } else {
                result.push_back(portalLeft);
                portalApex = portalLeft;
                apexIndex = leftIndex;
                portalLeft = portalApex;
                portalRight = portalApex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                index = apexIndex;
                continue;
            }
        }

        if (triArea2(portalApex, portalLeft, left) >= 0.0f) {
            if (nearlyEqualVec2(portalApex, portalLeft) || triArea2(portalApex, portalRight, left) < 0.0f) {
                portalLeft = left;
                leftIndex = index;
            } else {
                result.push_back(portalRight);
                portalApex = portalRight;
                apexIndex = rightIndex;
                portalLeft = portalApex;
                portalRight = portalApex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                index = apexIndex;
                continue;
            }
        }
    }

    if (result.empty() || !nearlyEqualVec2(result.back(), end)) {
        result.push_back(end);
    }
    return result;
}

void ensureCCW(std::vector<glm::vec2>& vertices) {
    if (vertices.size() < 3u) {
        return;
    }
    float twiceArea = 0.0f;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const std::size_t j = (i + 1u) % vertices.size();
        twiceArea += cross2(vertices[i], vertices[j]);
    }
    if (twiceArea < 0.0f) {
        std::reverse(vertices.begin(), vertices.end());
    }
}

std::vector<glm::vec2> clipConvexPolygons(
    const std::vector<glm::vec2>& subject,
    const std::vector<glm::vec2>& clip
) {
    if (subject.size() < 3u || clip.size() < 3u) {
        return {};
    }
    std::vector<glm::vec2> clipCCW = clip;
    ensureCCW(clipCCW);

    std::vector<glm::vec2> output = subject;
    for (std::size_t i = 0; i < clipCCW.size() && output.size() >= 3u; ++i) {
        const std::vector<glm::vec2> input = output;
        output.clear();
        const glm::vec2& edgeStart = clipCCW[i];
        const glm::vec2& edgeEnd = clipCCW[(i + 1u) % clipCCW.size()];
        const glm::vec2 edgeDir = edgeEnd - edgeStart;

        for (std::size_t j = 0; j < input.size(); ++j) {
            const glm::vec2& current = input[j];
            const glm::vec2& previous = input[(j + input.size() - 1u) % input.size()];
            const float currentSide = cross2(edgeDir, current - edgeStart);
            const float previousSide = cross2(edgeDir, previous - edgeStart);
            const bool currentInside = currentSide >= -kPolygonEpsilon;
            const bool previousInside = previousSide >= -kPolygonEpsilon;

            if (currentInside) {
                if (!previousInside) {
                    const glm::vec2 d = current - previous;
                    const float denom = cross2(edgeDir, d);
                    if (std::abs(denom) > kPlaneEpsilon) {
                        const float t = std::clamp(
                            cross2(edgeDir, edgeStart - previous) / denom, 0.0f, 1.0f);
                        output.push_back(previous + t * d);
                    }
                }
                output.push_back(current);
            } else if (previousInside) {
                const glm::vec2 d = current - previous;
                const float denom = cross2(edgeDir, d);
                if (std::abs(denom) > kPlaneEpsilon) {
                    const float t = std::clamp(
                        cross2(edgeDir, edgeStart - previous) / denom, 0.0f, 1.0f);
                    output.push_back(previous + t * d);
                }
            }
        }
    }
    return output;
}

std::vector<glm::vec2> clipConvexPolygonAgainstHalfPlane(
    const std::vector<glm::vec2>& polygon,
    const glm::vec2& lineA,
    const glm::vec2& lineB,
    bool keepLeft
) {
    if (polygon.size() < 3u) {
        return {};
    }

    const glm::vec2 lineDir = lineB - lineA;
    if (glm::length(lineDir) <= kPolygonEpsilon) {
        return polygon;
    }

    auto signedSide = [&](const glm::vec2& point) {
        const float side = cross2(lineDir, point - lineA);
        return keepLeft ? side : -side;
    };

    std::vector<glm::vec2> output{};
    output.reserve(polygon.size() + 1u);
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const glm::vec2& current = polygon[index];
        const glm::vec2& previous = polygon[(index + polygon.size() - 1u) % polygon.size()];
        const float currentSide = signedSide(current);
        const float previousSide = signedSide(previous);
        const bool currentInside = currentSide >= -kPolygonEpsilon;
        const bool previousInside = previousSide >= -kPolygonEpsilon;

        if (currentInside != previousInside) {
            const glm::vec2 segmentDir = current - previous;
            const float denominator = cross2(lineDir, segmentDir);
            if (std::abs(denominator) > kPlaneEpsilon) {
                const float t = std::clamp(cross2(lineDir, lineA - previous) / denominator, 0.0f, 1.0f);
                output.push_back(previous + t * segmentDir);
            }
        }
        if (currentInside) {
            output.push_back(current);
        }
    }

    return normalizePolygonVertices(output);
}

std::vector<std::vector<glm::vec2>> splitConvexPolygonByLine(
    const std::vector<glm::vec2>& polygon,
    const glm::vec2& lineA,
    const glm::vec2& lineB
) {
    if (polygon.size() < 3u || nearlyEqualVec2(lineA, lineB)) {
        return {polygon};
    }

    bool hasPositive = false;
    bool hasNegative = false;
    for (const glm::vec2& vertex : polygon) {
        const float side = cross2(lineB - lineA, vertex - lineA);
        hasPositive |= side > kPolygonEpsilon;
        hasNegative |= side < -kPolygonEpsilon;
    }
    if (!(hasPositive && hasNegative)) {
        return {polygon};
    }

    std::vector<glm::vec2> lhs = clipConvexPolygonAgainstHalfPlane(polygon, lineA, lineB, true);
    std::vector<glm::vec2> rhs = clipConvexPolygonAgainstHalfPlane(polygon, lineA, lineB, false);
    if (!polygonHasArea(lhs) || !polygonHasArea(rhs)) {
        return {polygon};
    }
    return {lhs, rhs};
}

std::string canonicalPolygonKey(const std::vector<glm::vec2>& vertices) {
    std::vector<glm::vec2> normalized = normalizePolygonVertices(vertices);
    if (!polygonHasArea(normalized)) {
        return {};
    }

    std::vector<QuantizedVec2> quantized{};
    quantized.reserve(normalized.size());
    for (const glm::vec2& vertex : normalized) {
        quantized.push_back(quantizeVec2(vertex));
    }

    std::size_t bestStart = 0u;
    for (std::size_t candidate = 1u; candidate < quantized.size(); ++candidate) {
        for (std::size_t offset = 0u; offset < quantized.size(); ++offset) {
            const QuantizedVec2& lhs = quantized[(candidate + offset) % quantized.size()];
            const QuantizedVec2& rhs = quantized[(bestStart + offset) % quantized.size()];
            if (lhs == rhs) {
                continue;
            }
            if (lhs < rhs) {
                bestStart = candidate;
            }
            break;
        }
    }

    std::ostringstream key{};
    for (std::size_t offset = 0u; offset < quantized.size(); ++offset) {
        const QuantizedVec2& vertex = quantized[(bestStart + offset) % quantized.size()];
        key << vertex.x << "," << vertex.y << ";";
    }
    return key.str();
}

std::vector<std::size_t> authoredPolygonsContainingPoint(const BakeLayerData& layer, const glm::vec2& point) {
    std::vector<std::size_t> containing{};
    for (const AuthoredBakePolygon& polygon : layer.polygons) {
        if (pointInOrOnPolygonXZ(point, polygon.verticesXZ)) {
            containing.push_back(polygon.assetIndex);
        }
    }
    return containing;
}

void appendUniqueIndices(std::vector<std::size_t>& dst, const std::vector<std::size_t>& src) {
    for (std::size_t value : src) {
        if (std::find(dst.begin(), dst.end(), value) == dst.end()) {
            dst.push_back(value);
        }
    }
}

std::vector<glm::vec2> buildConvexHull(std::vector<glm::vec2> points) {
    if (points.size() < 3u) {
        return {};
    }

    std::sort(points.begin(), points.end(), [](const glm::vec2& lhs, const glm::vec2& rhs) {
        if (lhs.x != rhs.x) {
            return lhs.x < rhs.x;
        }
        return lhs.y < rhs.y;
    });
    points.erase(std::unique(points.begin(), points.end(), [](const glm::vec2& lhs, const glm::vec2& rhs) {
        return nearlyEqualVec2(lhs, rhs);
    }), points.end());
    if (points.size() < 3u) {
        return {};
    }

    std::vector<glm::vec2> lower{};
    for (const glm::vec2& point : points) {
        while (lower.size() >= 2u &&
               triArea2(lower[lower.size() - 2u], lower.back(), point) <= kPolygonEpsilon) {
            lower.pop_back();
        }
        lower.push_back(point);
    }

    std::vector<glm::vec2> upper{};
    for (auto it = points.rbegin(); it != points.rend(); ++it) {
        while (upper.size() >= 2u &&
               triArea2(upper[upper.size() - 2u], upper.back(), *it) <= kPolygonEpsilon) {
            upper.pop_back();
        }
        upper.push_back(*it);
    }

    lower.pop_back();
    upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return normalizePolygonVertices(lower);
}

std::optional<std::vector<glm::vec2>> tryMergeConvexCells(
    const NavRuntimeCell& lhs,
    const NavRuntimeCell& rhs
) {
    if (std::abs(lhs.elevationY - rhs.elevationY) > kLayerGroupingEpsilon) {
        return std::nullopt;
    }
    if (!sharedPortal(lhs, rhs).has_value()) {
        return std::nullopt;
    }

    std::vector<glm::vec2> points = lhs.verticesXZ;
    for (const glm::vec2& vertex : rhs.verticesXZ) {
        if (std::find_if(points.begin(), points.end(), [&](const glm::vec2& existing) {
                return nearlyEqualVec2(existing, vertex);
            }) == points.end()) {
            points.push_back(vertex);
        }
    }

    std::vector<glm::vec2> hull = buildConvexHull(points);
    if (!polygonHasArea(hull)) {
        return std::nullopt;
    }

    const float mergedArea = std::abs(polygonSignedArea(hull));
    const float inputArea = std::abs(polygonSignedArea(lhs.verticesXZ)) + std::abs(polygonSignedArea(rhs.verticesXZ));
    if (std::abs(mergedArea - inputArea) > 0.001f) {
        return std::nullopt;
    }
    return hull;
}

void mergeAdjacentBakeCells(
    std::vector<NavRuntimeCell>& cells,
    std::vector<std::vector<std::size_t>>& cellToPolygonIndices
) {
    bool mergedAny = true;
    while (mergedAny) {
        mergedAny = false;
        for (std::size_t lhsIndex = 0; lhsIndex < cells.size() && !mergedAny; ++lhsIndex) {
            for (std::size_t rhsIndex = lhsIndex + 1u; rhsIndex < cells.size(); ++rhsIndex) {
                if (cellToPolygonIndices[lhsIndex] != cellToPolygonIndices[rhsIndex]) {
                    continue;
                }
                const auto merged = tryMergeConvexCells(cells[lhsIndex], cells[rhsIndex]);
                if (!merged.has_value()) {
                    continue;
                }
                cells[lhsIndex].verticesXZ = *merged;
                cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(rhsIndex));
                cellToPolygonIndices.erase(cellToPolygonIndices.begin() + static_cast<std::ptrdiff_t>(rhsIndex));
                mergedAny = true;
                break;
            }
        }
    }
}

std::vector<NavRuntimeCell> bakeLayerRuntimeCells(
    const BakeLayerData& layer,
    std::vector<std::vector<std::size_t>>& outCellToPolygonIndices
) {
    std::vector<std::pair<glm::vec2, glm::vec2>> splitLines{};
    splitLines.reserve(layer.triangles.size() * 3u);
    for (const BakedTriangle& triangle : layer.triangles) {
        for (std::size_t edgeIndex = 0; edgeIndex < 3u; ++edgeIndex) {
            splitLines.emplace_back(
                triangle.verticesXZ[edgeIndex],
                triangle.verticesXZ[(edgeIndex + 1u) % 3u]
            );
        }
    }

    std::unordered_map<std::string, std::size_t> cellIndexByKey{};
    std::vector<NavRuntimeCell> cells{};
    for (const BakedTriangle& triangle : layer.triangles) {
        std::vector<std::vector<glm::vec2>> fragments{
            {
                triangle.verticesXZ[0],
                triangle.verticesXZ[1],
                triangle.verticesXZ[2],
            }
        };

        for (const auto& [lineA, lineB] : splitLines) {
            std::vector<std::vector<glm::vec2>> nextFragments{};
            nextFragments.reserve(fragments.size() * 2u);
            for (const std::vector<glm::vec2>& fragment : fragments) {
                const std::vector<std::vector<glm::vec2>> split = splitConvexPolygonByLine(fragment, lineA, lineB);
                nextFragments.insert(nextFragments.end(), split.begin(), split.end());
            }
            fragments = std::move(nextFragments);
        }

        for (std::vector<glm::vec2>& fragment : fragments) {
            fragment = normalizePolygonVertices(fragment);
            if (!polygonHasArea(fragment)) {
                continue;
            }

            const glm::vec2 centroid = polygonCentroidXZ(fragment);
            std::vector<std::size_t> memberships = authoredPolygonsContainingPoint(layer, centroid);
            if (memberships.empty()) {
                continue;
            }
            std::sort(memberships.begin(), memberships.end());

            const std::string key = canonicalPolygonKey(fragment);
            if (key.empty()) {
                continue;
            }

            const auto [it, inserted] = cellIndexByKey.emplace(key, cells.size());
            if (inserted) {
                cells.push_back(NavRuntimeCell{layer.elevationY, fragment});
                outCellToPolygonIndices.push_back(std::move(memberships));
            } else {
                appendUniqueIndices(outCellToPolygonIndices[it->second], memberships);
                std::sort(outCellToPolygonIndices[it->second].begin(), outCellToPolygonIndices[it->second].end());
            }
        }
    }

    mergeAdjacentBakeCells(cells, outCellToPolygonIndices);
    return cells;
}

std::vector<std::size_t> findContainingCells(const NavigationRuntime& runtime, const glm::vec3& point) {
    std::vector<std::size_t> containing{};
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t index = 0; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        if (std::abs(point.y - cell.elevationY) > 1.0f) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            containing.push_back(index);
        }
    }
    return containing;
}

bool containsCellIndex(const std::vector<std::size_t>& indices, std::size_t cellIndex) {
    return std::find(indices.begin(), indices.end(), cellIndex) != indices.end();
}

std::optional<std::size_t> findNearestCell(const NavigationRuntime& runtime, const glm::vec3& point) {
    std::optional<std::size_t> bestIndex{};
    float bestDistance = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        const glm::vec2 center = index < runtime.bakedCellCenters.size()
            ? runtime.bakedCellCenters[index]
            : polygonCentroidXZ(cell.verticesXZ);
        const float planarDistance = glm::distance(center, glm::vec2(point.x, point.z));
        const float verticalDistance = std::abs(point.y - cell.elevationY);
        const float score = planarDistance + verticalDistance * 2.0f;
        if (score < bestDistance) {
            bestDistance = score;
            bestIndex = index;
        }
    }
    return bestIndex;
}

std::vector<std::size_t> findLinkEndpointCells(
    const NavigationRuntime& runtime,
    std::size_t authoredPolygonIndex,
    const glm::vec3& point
) {
    std::vector<std::size_t> containing{};
    if (authoredPolygonIndex >= runtime.polygonToCellIndices.size()) {
        return containing;
    }

    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t cellIndex : runtime.polygonToCellIndices[authoredPolygonIndex]) {
        if (cellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        if (std::abs(point.y - cell.elevationY) > 1.0f) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            containing.push_back(cellIndex);
        }
    }
    return containing;
}

std::optional<std::size_t> findNearestCandidateCell(
    const NavigationRuntime& runtime,
    const std::vector<std::size_t>& candidates,
    const glm::vec3& point
) {
    std::optional<std::size_t> bestIndex{};
    float bestScore = std::numeric_limits<float>::max();
    for (std::size_t cellIndex : candidates) {
        if (cellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const glm::vec2 center = cellIndex < runtime.bakedCellCenters.size()
            ? runtime.bakedCellCenters[cellIndex]
            : polygonCentroidXZ(runtime.bakedCells[cellIndex].verticesXZ);
        const float planarDistance = glm::distance(center, glm::vec2(point.x, point.z));
        const float verticalDistance = std::abs(point.y - runtime.bakedCells[cellIndex].elevationY);
        const float score = planarDistance + verticalDistance * 2.0f;
        if (score < bestScore) {
            bestScore = score;
            bestIndex = cellIndex;
        }
    }
    return bestIndex;
}

bool pointInsideAuthoredWalkableSurface(const NavigationRuntime& runtime, const glm::vec3& point) {
    const glm::vec2 pointXZ(point.x, point.z);
    for (const NavPolygon& polygon : runtime.asset.polygons) {
        if (std::abs(point.y - polygon.elevationY) > 1.0f) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, polygon.verticesXZ)) {
            return true;
        }
    }
    return false;
}

bool segmentInsideAuthoredWalkableSurface(
    const NavigationRuntime& runtime,
    const glm::vec3& from,
    const glm::vec3& to
) {
    constexpr int kSegmentSamples = 24;
    for (int sampleIndex = 0; sampleIndex <= kSegmentSamples; ++sampleIndex) {
        const float t = static_cast<float>(sampleIndex) / static_cast<float>(kSegmentSamples);
        if (!pointInsideAuthoredWalkableSurface(runtime, from + t * (to - from))) {
            return false;
        }
    }
    return true;
}

bool pathInsideAuthoredWalkableSurface(
    const NavigationRuntime& runtime,
    const glm::vec3& start,
    const std::vector<glm::vec3>& corners
) {
    glm::vec3 previous = start;
    for (const glm::vec3& corner : corners) {
        if (!segmentInsideAuthoredWalkableSurface(runtime, previous, corner)) {
            return false;
        }
        previous = corner;
    }
    return true;
}

bool segmentMatchesLink(
    const glm::vec3& from,
    const glm::vec3& to,
    const std::vector<const NavGraphEdge*>& edgePath,
    float epsilon
) {
    for (const NavGraphEdge* edge : edgePath) {
        if (edge == nullptr || !edge->viaLink) {
            continue;
        }
        if ((nearlyEqualVec3(from, edge->linkStartPoint, epsilon) && nearlyEqualVec3(to, edge->linkEndPoint, epsilon)) ||
            (nearlyEqualVec3(from, edge->linkEndPoint, epsilon) && nearlyEqualVec3(to, edge->linkStartPoint, epsilon))) {
            return true;
        }
    }
    return false;
}

bool pathInsideWalkableSurfaceOrLinks(
    const NavigationRuntime& runtime,
    const glm::vec3& start,
    const std::vector<glm::vec3>& corners,
    const std::vector<const NavGraphEdge*>& edgePath,
    float epsilon
) {
    glm::vec3 previous = start;
    for (const glm::vec3& corner : corners) {
        if (!segmentInsideAuthoredWalkableSurface(runtime, previous, corner) &&
            !segmentMatchesLink(previous, corner, edgePath, epsilon)) {
            return false;
        }
        previous = corner;
    }
    return true;
}

std::vector<glm::vec3> buildConservativeCellPathCorners(
    const NavigationRuntime& runtime,
    const glm::vec3& start,
    const glm::vec3& destination,
    const std::vector<std::size_t>& cellPath,
    const std::vector<const NavGraphEdge*>& edgePath,
    float arrivalRadius
) {
    std::vector<glm::vec3> corners{};
    if (cellPath.empty()) {
        return corners;
    }

    for (std::size_t stepIndex = 0; stepIndex < edgePath.size(); ++stepIndex) {
        const NavGraphEdge& edge = *edgePath[stepIndex];
        const std::size_t nextCellIndex = cellPath[stepIndex + 1u];
        if (edge.viaLink) {
            if (corners.empty() || !nearlyEqualVec3(corners.back(), edge.linkStartPoint, arrivalRadius)) {
                corners.push_back(edge.linkStartPoint);
            }
            corners.push_back(edge.linkEndPoint);
            if (stepIndex + 1u < edgePath.size()) {
                const glm::vec2 center = runtime.bakedCellCenters[nextCellIndex];
                corners.push_back(glm::vec3(center.x, runtime.bakedCells[nextCellIndex].elevationY, center.y));
            }
            continue;
        }
        if (stepIndex + 1u >= edgePath.size()) {
            continue;
        }
        const glm::vec2 center = runtime.bakedCellCenters[nextCellIndex];
        corners.push_back(glm::vec3(center.x, runtime.bakedCells[nextCellIndex].elevationY, center.y));
    }

    if (corners.empty() || !nearlyEqualVec3(corners.back(), destination, arrivalRadius)) {
        corners.push_back(destination);
    }
    return corners;
}

bool segmentMatchesAnyNavLink(
    const NavigationRuntime& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    float epsilon
) {
    for (const NavLink& link : runtime.asset.links) {
        if ((nearlyEqualVec3(from, link.fromPoint, epsilon) && nearlyEqualVec3(to, link.toPoint, epsilon)) ||
            (nearlyEqualVec3(from, link.toPoint, epsilon) && nearlyEqualVec3(to, link.fromPoint, epsilon))) {
            return true;
        }
    }
    return false;
}

std::string quantizedVec3Key(const glm::vec3& value) {
    const auto quantizeScalar = [](float component) {
        return static_cast<long long>(std::llround(static_cast<double>(component) * 10000.0));
    };
    return std::to_string(quantizeScalar(value.x)) + ":" +
        std::to_string(quantizeScalar(value.y)) + ":" +
        std::to_string(quantizeScalar(value.z));
}

std::optional<std::vector<glm::vec3>> findVisibilityPath(
    const NavigationRuntime& runtime,
    const glm::vec3& start,
    const glm::vec3& destination
) {
    std::vector<glm::vec3> nodes{};
    std::unordered_map<std::string, std::size_t> nodeIndexByKey{};
    const auto appendNode = [&](const glm::vec3& position) {
        const std::string key = quantizedVec3Key(position);
        const auto it = nodeIndexByKey.find(key);
        if (it != nodeIndexByKey.end()) {
            return it->second;
        }
        const std::size_t index = nodes.size();
        nodes.push_back(position);
        nodeIndexByKey[key] = index;
        return index;
    };

    const std::size_t startIndex = appendNode(start);
    const std::size_t destinationIndex = appendNode(destination);
    for (const NavRuntimeCell& cell : runtime.bakedCells) {
        for (const glm::vec2& vertex : cell.verticesXZ) {
            appendNode(glm::vec3(vertex.x, cell.elevationY, vertex.y));
        }
    }

    struct LinkNodePair {
        std::size_t fromIndex{0u};
        std::size_t toIndex{0u};
        bool bidirectional{true};
    };

    std::vector<LinkNodePair> linkNodes{};
    linkNodes.reserve(runtime.asset.links.size());
    for (const NavLink& link : runtime.asset.links) {
        linkNodes.push_back(LinkNodePair{
            appendNode(link.fromPoint),
            appendNode(link.toPoint),
            link.bidirectional
        });
    }

    std::vector<std::vector<std::pair<std::size_t, float>>> graph(nodes.size());
    for (std::size_t lhsIndex = 0; lhsIndex < nodes.size(); ++lhsIndex) {
        for (std::size_t rhsIndex = lhsIndex + 1u; rhsIndex < nodes.size(); ++rhsIndex) {
            if (std::abs(nodes[lhsIndex].y - nodes[rhsIndex].y) > kLayerGroupingEpsilon) {
                continue;
            }
            if (!segmentInsideAuthoredWalkableSurface(runtime, nodes[lhsIndex], nodes[rhsIndex])) {
                continue;
            }
            const float distance = glm::distance(nodes[lhsIndex], nodes[rhsIndex]);
            graph[lhsIndex].push_back(std::pair<std::size_t, float>{rhsIndex, distance});
            graph[rhsIndex].push_back(std::pair<std::size_t, float>{lhsIndex, distance});
        }
    }

    for (const LinkNodePair& link : linkNodes) {
        const float distance = glm::distance(nodes[link.fromIndex], nodes[link.toIndex]);
        graph[link.fromIndex].push_back(std::pair<std::size_t, float>{link.toIndex, distance});
        if (link.bidirectional) {
            graph[link.toIndex].push_back(std::pair<std::size_t, float>{link.fromIndex, distance});
        }
    }

    struct QueueItem {
        float distance{0.0f};
        std::size_t nodeIndex{0u};

        bool operator<(const QueueItem& other) const {
            return distance > other.distance;
        }
    };

    std::vector<float> distances(nodes.size(), std::numeric_limits<float>::max());
    std::vector<int> parents(nodes.size(), -1);
    std::priority_queue<QueueItem> open{};
    distances[startIndex] = 0.0f;
    open.push(QueueItem{0.0f, startIndex});

    while (!open.empty()) {
        const QueueItem current = open.top();
        open.pop();
        if (current.distance > distances[current.nodeIndex] + kPolygonEpsilon) {
            continue;
        }
        if (current.nodeIndex == destinationIndex) {
            break;
        }
        for (const auto& [neighborIndex, edgeDistance] : graph[current.nodeIndex]) {
            const float candidate = distances[current.nodeIndex] + edgeDistance;
            if (candidate >= distances[neighborIndex]) {
                continue;
            }
            distances[neighborIndex] = candidate;
            parents[neighborIndex] = static_cast<int>(current.nodeIndex);
            open.push(QueueItem{candidate, neighborIndex});
        }
    }

    if (startIndex != destinationIndex && parents[destinationIndex] < 0) {
        return std::nullopt;
    }

    std::vector<glm::vec3> path{};
    for (std::size_t currentIndex = destinationIndex;; currentIndex = static_cast<std::size_t>(parents[currentIndex])) {
        path.push_back(nodes[currentIndex]);
        if (currentIndex == startIndex) {
            break;
        }
    }
    std::reverse(path.begin(), path.end());

    std::vector<glm::vec3> simplified{path.front()};
    std::size_t currentIndex = 0u;
    while (currentIndex + 1u < path.size()) {
        std::size_t nextIndex = path.size() - 1u;
        for (; nextIndex > currentIndex + 1u; --nextIndex) {
            if (segmentInsideAuthoredWalkableSurface(runtime, path[currentIndex], path[nextIndex]) ||
                segmentMatchesAnyNavLink(runtime, path[currentIndex], path[nextIndex], 0.01f)) {
                break;
            }
        }
        simplified.push_back(path[nextIndex]);
        currentIndex = nextIndex;
    }
    simplified.erase(simplified.begin());
    return simplified;
}

void appendGraphEdgeIfMissing(
    std::vector<NavGraphEdge>& edges,
    const NavGraphEdge& candidate
) {
    const auto it = std::find_if(edges.begin(), edges.end(), [&](const NavGraphEdge& existing) {
        return existing.targetCellIndex == candidate.targetCellIndex &&
            existing.viaLink == candidate.viaLink &&
            existing.linkId == candidate.linkId &&
            nearlyEqualVec2(existing.portalA, candidate.portalA) &&
            nearlyEqualVec2(existing.portalB, candidate.portalB) &&
            nearlyEqualVec3(existing.linkStartPoint, candidate.linkStartPoint) &&
            nearlyEqualVec3(existing.linkEndPoint, candidate.linkEndPoint);
    });
    if (it == edges.end()) {
        edges.push_back(candidate);
    }
}

int nextPolygonId(const NavMeshAsset& asset) {
    int nextId = 1;
    for (const NavPolygon& polygon : asset.polygons) {
        nextId = std::max(nextId, polygon.id + 1);
    }
    return nextId;
}

int nextLinkId(const NavMeshAsset& asset) {
    int nextId = 1;
    for (const NavLink& link : asset.links) {
        nextId = std::max(nextId, link.id + 1);
    }
    return nextId;
}

void setRuntimeStatus(NavigationRuntime& runtime, std::string message, bool isError) {
    runtime.statusMessage = std::move(message);
    runtime.statusIsError = isError;
}

std::optional<std::size_t> findPolygonIndexById(const NavigationRuntime& runtime, int polygonId) {
    const auto it = runtime.polygonIndexById.find(polygonId);
    if (it == runtime.polygonIndexById.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::size_t> findPolygonIndexById(const NavMeshAsset& asset, int polygonId) {
    for (std::size_t index = 0; index < asset.polygons.size(); ++index) {
        if (asset.polygons[index].id == polygonId) {
            return index;
        }
    }
    return std::nullopt;
}

glm::vec3 cellCenter3(const NavigationRuntime& runtime, std::size_t cellIndex) {
    const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
    const glm::vec2 center = cellIndex < runtime.bakedCellCenters.size()
        ? runtime.bakedCellCenters[cellIndex]
        : polygonCentroidXZ(cell.verticesXZ);
    return glm::vec3(center.x, cell.elevationY, center.y);
}

void updateSourceOverride(
    std::vector<NavSourceTagOverride>& overrides,
    const NavSourceComponent& source,
    NavSourceTag appliedTag
) {
    const auto it = std::find_if(overrides.begin(), overrides.end(), [&source](const NavSourceTagOverride& overrideRecord) {
        return overrideRecord.stableId == source.stableId;
    });
    if (appliedTag == source.defaultTag) {
        if (it != overrides.end()) {
            overrides.erase(it);
        }
        return;
    }

    if (it != overrides.end()) {
        it->tag = appliedTag;
        return;
    }
    overrides.push_back(NavSourceTagOverride{source.stableId, appliedTag});
}

bool shortestYawStep(float currentYaw, float desiredYaw, float maxStep, float& outYaw) {
    float delta = std::fmod(desiredYaw - currentYaw, 360.0f);
    if (delta > 180.0f) {
        delta -= 360.0f;
    } else if (delta < -180.0f) {
        delta += 360.0f;
    }
    if (std::abs(delta) <= maxStep) {
        outYaw = desiredYaw;
        return true;
    }
    outYaw = currentYaw + std::copysign(maxStep, delta);
    return false;
}

int findClipContaining(const render::GltfModelData& model, std::string_view token) {
    const std::string loweredToken = lowercaseCopy(std::string(token));
    for (std::size_t clipIndex = 0; clipIndex < model.animations.size(); ++clipIndex) {
        std::string lowered = lowercaseCopy(model.animations[clipIndex].name);
        if (lowered.find(loweredToken) != std::string::npos) {
            return static_cast<int>(clipIndex);
        }
    }
    return -1;
}

std::string resolveNavAssetPath(const std::string& assetPath) {
    if (assetPath.empty()) {
        return {};
    }
    const std::filesystem::path path(assetPath);
    if (path.is_absolute()) {
        return path.string();
    }

    char* basePath = SDL_GetBasePath();
    std::string resolved = basePath ? (std::filesystem::path(basePath) / path).string() : path.string();
    if (basePath != nullptr) {
        SDL_free(basePath);
    }
    return resolved;
}

}  // namespace

const char* navSourceTagName(NavSourceTag tag) {
    switch (tag) {
        case NavSourceTag::Walkable:
            return "Walkable";
        case NavSourceTag::Blocking:
            return "Blocking";
        case NavSourceTag::Ignored:
        default:
            return "Ignored";
    }
}

bool tryParseNavSourceTag(const std::string& token, NavSourceTag& outTag) {
    const std::string lowered = lowercaseCopy(token);
    if (lowered == "walkable") {
        outTag = NavSourceTag::Walkable;
        return true;
    }
    if (lowered == "blocking") {
        outTag = NavSourceTag::Blocking;
        return true;
    }
    if (lowered == "ignored") {
        outTag = NavSourceTag::Ignored;
        return true;
    }
    return false;
}

std::string serializeNavMeshAsset(const NavMeshAsset& asset) {
    std::ostringstream out;
    out << "version " << asset.version << "\n";
    for (const NavSourceTagOverride& overrideRecord : asset.sourceTagOverrides) {
        out << "source_tag_override " << std::quoted(overrideRecord.stableId) << " " << navSourceTagName(overrideRecord.tag) << "\n";
    }
    for (const NavPolygon& polygon : asset.polygons) {
        out << "polygon " << polygon.id << " " << std::fixed << std::setprecision(4) << polygon.elevationY;
        for (const glm::vec2& vertex : polygon.verticesXZ) {
            out << " " << vertex.x << " " << vertex.y;
        }
        out << "\n";
    }
    for (const NavLink& link : asset.links) {
        out << "link " << link.id << " " << link.fromPolygonId << " " << link.toPolygonId
            << " " << std::fixed << std::setprecision(4)
            << link.fromPoint.x << " " << link.fromPoint.y << " " << link.fromPoint.z
            << " " << link.toPoint.x << " " << link.toPoint.y << " " << link.toPoint.z
            << " " << (link.bidirectional ? 1 : 0) << "\n";
    }
    return out.str();
}

bool parseNavMeshAsset(const std::string& text, NavMeshAsset& outAsset, std::string* error) {
    std::istringstream input(text);
    std::string line{};
    NavMeshAsset asset{};
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream lineStream(line);
        std::string keyword{};
        lineStream >> keyword;
        if (keyword == "version") {
            if (!(lineStream >> asset.version)) {
                if (error) {
                    *error = "Invalid version at line " + std::to_string(lineNumber);
                }
                return false;
            }
        } else if (keyword == "source_tag_override") {
            std::string stableId{};
            std::string tagToken{};
            if (!(lineStream >> std::quoted(stableId) >> tagToken)) {
                if (error) {
                    *error = "Invalid source_tag_override at line " + std::to_string(lineNumber);
                }
                return false;
            }
            NavSourceTag tag = NavSourceTag::Ignored;
            if (!tryParseNavSourceTag(tagToken, tag)) {
                if (error) {
                    *error = "Unknown nav tag at line " + std::to_string(lineNumber);
                }
                return false;
            }
            asset.sourceTagOverrides.push_back(NavSourceTagOverride{stableId, tag});
        } else if (keyword == "polygon") {
            NavPolygon polygon{};
            if (!(lineStream >> polygon.id >> polygon.elevationY)) {
                if (error) {
                    *error = "Invalid polygon header at line " + std::to_string(lineNumber);
                }
                return false;
            }
            glm::vec2 vertex(0.0f);
            while (lineStream >> vertex.x >> vertex.y) {
                polygon.verticesXZ.push_back(vertex);
            }
            if (!polygonValid(polygon)) {
                if (error) {
                    *error = "Polygon must have at least three vertices at line " + std::to_string(lineNumber);
                }
                return false;
            }
            asset.polygons.push_back(std::move(polygon));
        } else if (keyword == "link") {
            NavLink link{};
            int bidirectional = 0;
            if (!(lineStream >> link.id >> link.fromPolygonId >> link.toPolygonId
                    >> link.fromPoint.x >> link.fromPoint.y >> link.fromPoint.z
                    >> link.toPoint.x >> link.toPoint.y >> link.toPoint.z
                    >> bidirectional)) {
                if (error) {
                    *error = "Invalid link at line " + std::to_string(lineNumber);
                }
                return false;
            }
            link.bidirectional = bidirectional != 0;
            asset.links.push_back(link);
        } else {
            if (error) {
                *error = "Unknown keyword '" + keyword + "' at line " + std::to_string(lineNumber);
            }
            return false;
        }
    }

    if (asset.version != kNavAssetVersion) {
        if (error) {
            *error = "Unsupported navmesh version " + std::to_string(asset.version);
        }
        return false;
    }

    outAsset = std::move(asset);
    return true;
}

bool NavigationSystem::initializeScene(const SceneBlueprint& blueprint, World& world, NavigationRuntime& runtime) const {
    runtime.assetPath = resolveNavAssetPath(blueprint.navMeshAssetPath);
    runtime.editor = NavigationEditorState{};
    const bool loaded = loadAsset(runtime);
    const ParentPathData paths = buildStableIdPaths(world);

    const std::unordered_map<std::string, NavSourceTag> overrideById = [&runtime]() {
        std::unordered_map<std::string, NavSourceTag> overrides{};
        overrides.reserve(runtime.asset.sourceTagOverrides.size());
        for (const NavSourceTagOverride& overrideRecord : runtime.asset.sourceTagOverrides) {
            overrides[overrideRecord.stableId] = overrideRecord.tag;
        }
        return overrides;
    }();

    for (EntityId entity : world.renderables.entities()) {
        const RenderableComponent& renderable = world.renderables.get(entity);
        const NavSourceTag defaultTag = defaultTagForLayer(renderable.layer);
        const auto pathIt = paths.pathByEntity.find(entity);
        const std::string stableId = pathIt != paths.pathByEntity.end()
            ? pathIt->second
            : ("Renderable/" + std::to_string(entity.index));
        const auto overrideIt = overrideById.find(stableId);
        const NavSourceTag effectiveTag = overrideIt != overrideById.end() ? overrideIt->second : defaultTag;
        world.navSources.emplace(entity, NavSourceComponent{stableId, defaultTag, effectiveTag});
    }

    for (EntityId entity : world.animatedModels.entities()) {
        const NameComponent* name = world.names.tryGet(entity);
        if (name == nullptr || name->value != "Character") {
            continue;
        }
        if (!world.navAgents.contains(entity)) {
            world.navAgents.emplace(entity, NavAgentComponent{});
        }
        const AnimatedModelComponent& animated = world.animatedModels.get(entity);
        if (animated.model) {
            const int idleClip = render::findDefaultAnimationClipIndex(*animated.model);
            int walkClip = findClipContaining(*animated.model, "walk");
            if (walkClip < 0) {
                walkClip = findClipContaining(*animated.model, "run");
            }
            world.locomotion.emplace(entity, LocomotionComponent{idleClip, walkClip});
        }
    }

    const bool rebuilt = rebuildRuntime(runtime);
    return loaded && rebuilt;
}

bool NavigationSystem::loadAsset(NavigationRuntime& runtime) const {
    runtime.asset = NavMeshAsset{};
    if (runtime.assetPath.empty()) {
        setRuntimeStatus(runtime, "No navmesh asset configured.", true);
        return false;
    }

    std::ifstream input(runtime.assetPath, std::ios::binary);
    if (!input.is_open()) {
        runtime.asset.version = kNavAssetVersion;
        setRuntimeStatus(runtime, "Navmesh asset missing; using an empty in-memory asset.", false);
        return true;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string error{};
    if (!parseNavMeshAsset(buffer.str(), runtime.asset, &error)) {
        runtime.asset = NavMeshAsset{};
        setRuntimeStatus(runtime, error, true);
        return false;
    }
    setRuntimeStatus(runtime, "Loaded navmesh asset.", false);
    return true;
}

bool NavigationSystem::reloadAsset(NavigationRuntime& runtime) const {
    if (!loadAsset(runtime)) {
        return false;
    }
    return rebuildRuntime(runtime);
}

bool NavigationSystem::saveAsset(const NavigationRuntime& runtime, std::string* error) const {
    if (runtime.assetPath.empty()) {
        if (error) {
            *error = "No navmesh asset path configured.";
        }
        return false;
    }

    std::error_code createError{};
    const std::filesystem::path path(runtime.assetPath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), createError);
        if (createError) {
            if (error) {
                *error = createError.message();
            }
            return false;
        }
    }

    std::ofstream output(runtime.assetPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        if (error) {
            *error = "Failed to open navmesh asset for writing.";
        }
        return false;
    }
    output << serializeNavMeshAsset(runtime.asset);
    return output.good();
}

bool NavigationSystem::rebuildRuntime(NavigationRuntime& runtime, std::string* error) const {
    runtime.polygonIndexById.clear();
    runtime.polygonCenters.clear();
    runtime.bakedCells.clear();
    runtime.bakedCellCenters.clear();
    runtime.polygonToCellIndices.clear();
    runtime.cellToPolygonIndices.clear();
    runtime.graph.clear();

    std::vector<BakeLayerData> bakeLayers{};
    runtime.polygonToCellIndices.resize(runtime.asset.polygons.size());
    for (std::size_t index = 0; index < runtime.asset.polygons.size(); ++index) {
        const NavPolygon& polygon = runtime.asset.polygons[index];
        if (!polygonValid(polygon)) {
            if (error) {
                *error = "Invalid polygon geometry for polygon id " + std::to_string(polygon.id);
            }
            return false;
        }
        runtime.polygonIndexById[polygon.id] = index;
        runtime.polygonCenters.push_back(polygonCentroidXZ(polygon.verticesXZ));

        std::vector<glm::vec2> normalized = normalizePolygonVertices(polygon.verticesXZ);
        if (!polygonIsSimple(normalized)) {
            if (error) {
                *error = "Self-intersecting polygon geometry for polygon id " + std::to_string(polygon.id);
            }
            return false;
        }
        if (!polygonHasArea(normalized)) {
            if (error) {
                *error = "Degenerate polygon geometry for polygon id " + std::to_string(polygon.id);
            }
            return false;
        }

        const std::vector<std::array<glm::vec2, 3u>> triangles = triangulateSimplePolygon(normalized);
        if (triangles.empty()) {
            if (error) {
                *error = "Failed to triangulate polygon id " + std::to_string(polygon.id);
            }
            return false;
        }

        auto layerIt = std::find_if(bakeLayers.begin(), bakeLayers.end(), [&](const BakeLayerData& layer) {
            return std::abs(layer.elevationY - polygon.elevationY) <= kLayerGroupingEpsilon;
        });
        if (layerIt == bakeLayers.end()) {
            bakeLayers.push_back(BakeLayerData{polygon.elevationY});
            layerIt = bakeLayers.end() - 1;
        }

        layerIt->polygons.push_back(AuthoredBakePolygon{index, polygon.elevationY, normalized});
        for (const auto& triangle : triangles) {
            layerIt->triangles.push_back(BakedTriangle{
                index,
                polygon.elevationY,
                triangle
            });
        }
    }

    for (const BakeLayerData& layer : bakeLayers) {
        std::vector<std::vector<std::size_t>> layerCellToPolygonIndices{};
        std::vector<NavRuntimeCell> layerCells = bakeLayerRuntimeCells(layer, layerCellToPolygonIndices);
        for (std::size_t localCellIndex = 0; localCellIndex < layerCells.size(); ++localCellIndex) {
            const std::size_t globalCellIndex = runtime.bakedCells.size();
            runtime.bakedCellCenters.push_back(polygonCentroidXZ(layerCells[localCellIndex].verticesXZ));
            runtime.bakedCells.push_back(std::move(layerCells[localCellIndex]));
            runtime.cellToPolygonIndices.push_back(layerCellToPolygonIndices[localCellIndex]);
            for (std::size_t polygonIndex : runtime.cellToPolygonIndices.back()) {
                if (polygonIndex < runtime.polygonToCellIndices.size()) {
                    runtime.polygonToCellIndices[polygonIndex].push_back(globalCellIndex);
                }
            }
        }
    }

    runtime.graph.resize(runtime.bakedCells.size());
    for (std::size_t lhsIndex = 0; lhsIndex < runtime.bakedCells.size(); ++lhsIndex) {
        const NavRuntimeCell& lhs = runtime.bakedCells[lhsIndex];
        for (std::size_t rhsIndex = lhsIndex + 1u; rhsIndex < runtime.bakedCells.size(); ++rhsIndex) {
            const NavRuntimeCell& rhs = runtime.bakedCells[rhsIndex];
            if (std::abs(lhs.elevationY - rhs.elevationY) > kLayerGroupingEpsilon) {
                continue;
            }
            const auto portal = sharedPortal(lhs, rhs);
            if (!portal.has_value()) {
                continue;
            }
            runtime.graph[lhsIndex].push_back(NavGraphEdge{
                rhsIndex,
                false,
                -1,
                portal->a,
                portal->b,
                glm::vec3(0.0f),
                glm::vec3(0.0f)
            });
            runtime.graph[rhsIndex].push_back(NavGraphEdge{
                lhsIndex,
                false,
                -1,
                portal->a,
                portal->b,
                glm::vec3(0.0f),
                glm::vec3(0.0f)
            });
        }
    }

    for (const NavLink& link : runtime.asset.links) {
        const auto fromIndex = findPolygonIndexById(runtime, link.fromPolygonId);
        const auto toIndex = findPolygonIndexById(runtime, link.toPolygonId);
        if (!fromIndex.has_value() || !toIndex.has_value()) {
            continue;
        }
        std::vector<std::size_t> fromCells = findLinkEndpointCells(runtime, *fromIndex, link.fromPoint);
        std::vector<std::size_t> toCells = findLinkEndpointCells(runtime, *toIndex, link.toPoint);
        if (fromCells.empty()) {
            if (const auto fallback = findNearestCandidateCell(runtime, runtime.polygonToCellIndices[*fromIndex], link.fromPoint);
                fallback.has_value()) {
                fromCells.push_back(*fallback);
            }
        }
        if (toCells.empty()) {
            if (const auto fallback = findNearestCandidateCell(runtime, runtime.polygonToCellIndices[*toIndex], link.toPoint);
                fallback.has_value()) {
                toCells.push_back(*fallback);
            }
        }
        for (std::size_t fromCellIndex : fromCells) {
            for (std::size_t toCellIndex : toCells) {
                appendGraphEdgeIfMissing(runtime.graph[fromCellIndex], NavGraphEdge{
                    toCellIndex,
                    true,
                    link.id,
                    glm::vec2(0.0f),
                    glm::vec2(0.0f),
                    link.fromPoint,
                    link.toPoint
                });
                if (link.bidirectional) {
                    appendGraphEdgeIfMissing(runtime.graph[toCellIndex], NavGraphEdge{
                        fromCellIndex,
                        true,
                        link.id,
                        glm::vec2(0.0f),
                        glm::vec2(0.0f),
                        link.toPoint,
                        link.fromPoint
                    });
                }
            }
        }
    }
    return true;
}

bool NavigationSystem::generateFromTags(const World& world, NavigationRuntime& runtime, std::string* error) const {
    std::vector<LayerBuildData> layers{};
    std::vector<BlockingFootprint> blockers{};

    for (EntityId entity : world.navSources.entities()) {
        const NavSourceComponent& source = world.navSources.get(entity);
        const NavSourceGeometryComponent* geometry = world.navSourceGeometry.tryGet(entity);
        if (geometry == nullptr || !geometry->mesh) {
            continue;
        }
        if (entity.index >= world.transformCache_.size()) {
            continue;
        }
        const glm::mat4 worldMatrix = world.transformCache_[entity.index].worldMatrix;
        const render::Mesh& mesh = *geometry->mesh;
        if (mesh.indices.size() % 3u != 0u) {
            continue;
        }

        if (source.effectiveTag == NavSourceTag::Walkable) {
            for (std::size_t index = 0; index + 2u < mesh.indices.size(); index += 3u) {
                const unsigned int ia = mesh.indices[index];
                const unsigned int ib = mesh.indices[index + 1u];
                const unsigned int ic = mesh.indices[index + 2u];
                if (ia >= mesh.positions.size() || ib >= mesh.positions.size() || ic >= mesh.positions.size()) {
                    continue;
                }

                const glm::vec3 a = transformPoint3(worldMatrix, mesh.positions[ia]);
                const glm::vec3 b = transformPoint3(worldMatrix, mesh.positions[ib]);
                const glm::vec3 c = transformPoint3(worldMatrix, mesh.positions[ic]);
                const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
                if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z)) {
                    continue;
                }
                if (normal.y < kHorizontalNormalMinDot) {
                    continue;
                }
                const float elevation = (a.y + b.y + c.y) / 3.0f;
                addTriangleToLayer(
                    layers,
                    elevation,
                    WalkableTriangle{
                        elevation,
                        glm::vec2(a.x, a.z),
                        glm::vec2(b.x, b.z),
                        glm::vec2(c.x, c.z)
                    }
                );
            }
        } else if (source.effectiveTag == NavSourceTag::Blocking) {
            if (const std::optional<BlockingFootprint> footprint = computeFootprint(mesh, worldMatrix); footprint.has_value()) {
                blockers.push_back(*footprint);
            }
        }
    }

    for (LayerBuildData& layer : layers) {
        layer.blockers = blockers;
    }

    runtime.asset.polygons.clear();
    runtime.asset.links.clear();
    int polygonId = 1;
    std::sort(layers.begin(), layers.end(), [](const LayerBuildData& lhs, const LayerBuildData& rhs) {
        return lhs.elevationY < rhs.elevationY;
    });
    for (const LayerBuildData& layer : layers) {
        std::vector<NavPolygon> polygons = buildPolygonsForLayer(layer, polygonId);
        runtime.asset.polygons.insert(
            runtime.asset.polygons.end(),
            std::make_move_iterator(polygons.begin()),
            std::make_move_iterator(polygons.end())
        );
    }

    if (!rebuildRuntime(runtime, error)) {
        return false;
    }
    setRuntimeStatus(runtime, "Generated navmesh polygons from tagged sources.", false);
    return true;
}

std::optional<NavHitResult> NavigationSystem::hitTest(
    const NavigationRuntime& runtime,
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight,
    int mouseX,
    int mouseY
) const {
    const std::optional<ViewportRay> ray = makeViewportRay(camera, viewportWidth, viewportHeight, mouseX, mouseY);
    if (!ray.has_value()) {
        return std::nullopt;
    }

    std::optional<NavHitResult> bestHit{};
    for (const NavPolygon& polygon : runtime.asset.polygons) {
        if (std::abs(ray->direction.y) <= kPlaneEpsilon) {
            continue;
        }
        const float t = (polygon.elevationY - ray->origin.y) / ray->direction.y;
        if (t < 0.0f) {
            continue;
        }
        const glm::vec3 point = ray->origin + ray->direction * t;
        if (!pointInPolygonXZ(glm::vec2(point.x, point.z), polygon.verticesXZ)) {
            continue;
        }
        if (!bestHit.has_value() || t < bestHit->distance) {
            bestHit = NavHitResult{polygon.id, point, t};
        }
    }
    return bestHit;
}

bool NavigationSystem::setAgentDestination(
    World& world,
    const NavigationRuntime& runtime,
    EntityId agentEntity,
    const glm::vec3& destination
) const {
    NavAgentComponent* agent = world.navAgents.tryGet(agentEntity);
    TransformComponent* transform = world.transforms.tryGet(agentEntity);
    if (agent == nullptr || transform == nullptr || runtime.bakedCells.empty()) {
        return false;
    }

    std::vector<std::size_t> startCells = findContainingCells(runtime, transform->position);
    std::vector<std::size_t> targetCells = findContainingCells(runtime, destination);
    if (!startCells.empty() && !targetCells.empty()) {
        if (const auto visibilityPath = findVisibilityPath(runtime, transform->position, destination);
            visibilityPath.has_value()) {
            agent->pathCorners = *visibilityPath;
            agent->destination = destination;
            agent->moving = !agent->pathCorners.empty();
            return true;
        }
    }
    if (startCells.empty()) {
        if (const auto nearest = findNearestCell(runtime, transform->position); nearest.has_value()) {
            startCells.push_back(*nearest);
        }
    }
    if (targetCells.empty()) {
        if (const auto nearest = findNearestCell(runtime, destination); nearest.has_value()) {
            targetCells.push_back(*nearest);
        }
    }
    if (startCells.empty() || targetCells.empty()) {
        return false;
    }

    struct QueueItem {
        float fScore{0.0f};
        float gScore{0.0f};
        std::size_t cellIndex{0u};

        bool operator<(const QueueItem& other) const {
            return fScore > other.fScore;
        }
    };

    const std::size_t cellCount = runtime.bakedCells.size();
    std::vector<float> gScores(cellCount, std::numeric_limits<float>::max());
    std::vector<int> parents(cellCount, -1);
    std::vector<int> parentEdgeIndices(cellCount, -1);
    std::vector<std::uint8_t> isStartCell(cellCount, 0u);
    std::vector<std::uint8_t> isTargetCell(cellCount, 0u);
    std::priority_queue<QueueItem> open{};

    for (std::size_t cellIndex : startCells) {
        isStartCell[cellIndex] = 1u;
    }
    for (std::size_t cellIndex : targetCells) {
        isTargetCell[cellIndex] = 1u;
    }

    const auto heuristicForCell = [&](std::size_t cellIndex) {
        const glm::vec3 cellCenter = cellCenter3(runtime, cellIndex);
        float bestDistance = std::numeric_limits<float>::max();
        for (std::size_t targetCellIndex : targetCells) {
            bestDistance = std::min(bestDistance, glm::distance(cellCenter, cellCenter3(runtime, targetCellIndex)));
        }
        return bestDistance == std::numeric_limits<float>::max() ? 0.0f : bestDistance;
    };

    for (std::size_t cellIndex : startCells) {
        gScores[cellIndex] = 0.0f;
        open.push(QueueItem{heuristicForCell(cellIndex), 0.0f, cellIndex});
    }

    std::optional<std::size_t> reachedTarget{};

    while (!open.empty()) {
        const QueueItem current = open.top();
        open.pop();
        if (current.gScore > gScores[current.cellIndex] + kPolygonEpsilon) {
            continue;
        }
        if (isTargetCell[current.cellIndex] != 0u) {
            reachedTarget = current.cellIndex;
            break;
        }

        for (std::size_t edgeIndex = 0; edgeIndex < runtime.graph[current.cellIndex].size(); ++edgeIndex) {
            const NavGraphEdge& edge = runtime.graph[current.cellIndex][edgeIndex];
            const glm::vec3 fromCenter = cellCenter3(runtime, current.cellIndex);
            const glm::vec3 toCenter = cellCenter3(runtime, edge.targetCellIndex);
            const float stepCost = edge.viaLink
                ? glm::distance(edge.linkStartPoint, edge.linkEndPoint)
                : glm::distance(fromCenter, toCenter);
            const float candidate = gScores[current.cellIndex] + stepCost;
            if (candidate >= gScores[edge.targetCellIndex]) {
                continue;
            }
            gScores[edge.targetCellIndex] = candidate;
            parents[edge.targetCellIndex] = static_cast<int>(current.cellIndex);
            parentEdgeIndices[edge.targetCellIndex] = static_cast<int>(edgeIndex);
            const float heuristic = heuristicForCell(edge.targetCellIndex);
            open.push(QueueItem{candidate + heuristic, candidate, edge.targetCellIndex});
        }
    }

    if (!reachedTarget.has_value()) {
        return false;
    }

    std::vector<std::size_t> cellPath{};
    std::vector<const NavGraphEdge*> edgePath{};
    std::size_t currentCell = *reachedTarget;
    cellPath.push_back(currentCell);
    while (isStartCell[currentCell] == 0u) {
        const int parentCell = parents[currentCell];
        const int edgeIndex = parentEdgeIndices[currentCell];
        if (parentCell < 0 || edgeIndex < 0) {
            return false;
        }
        const NavGraphEdge& edge = runtime.graph[static_cast<std::size_t>(parentCell)][static_cast<std::size_t>(edgeIndex)];
        edgePath.push_back(&edge);
        cellPath.push_back(static_cast<std::size_t>(parentCell));
        currentCell = static_cast<std::size_t>(parentCell);
    }
    std::reverse(cellPath.begin(), cellPath.end());
    std::reverse(edgePath.begin(), edgePath.end());

    std::vector<glm::vec3> corners{};
    glm::vec3 segmentStart = transform->position;
    std::vector<std::pair<glm::vec2, glm::vec2>> portals{};
    float currentLayerY = runtime.bakedCells[cellPath.front()].elevationY;
    for (std::size_t stepIndex = 0; stepIndex < edgePath.size(); ++stepIndex) {
        const NavGraphEdge& edge = *edgePath[stepIndex];
        const std::size_t fromCellIndex = cellPath[stepIndex];
        const std::size_t toCellIndex = cellPath[stepIndex + 1u];
        if (!edge.viaLink) {
            const glm::vec2 left = orientPortalLeft(
                runtime.bakedCellCenters[fromCellIndex],
                runtime.bakedCellCenters[toCellIndex],
                edge.portalA,
                edge.portalB
            );
            const glm::vec2 right = orientPortalRight(
                runtime.bakedCellCenters[fromCellIndex],
                runtime.bakedCellCenters[toCellIndex],
                edge.portalA,
                edge.portalB
            );
            portals.emplace_back(left, right);
            continue;
        }

        const std::vector<glm::vec2> pulled = stringPull(
            glm::vec2(segmentStart.x, segmentStart.z),
            portals,
            glm::vec2(edge.linkStartPoint.x, edge.linkStartPoint.z)
        );
        for (std::size_t pointIndex = 1; pointIndex < pulled.size(); ++pointIndex) {
            corners.push_back(glm::vec3(pulled[pointIndex].x, currentLayerY, pulled[pointIndex].y));
        }
        corners.push_back(edge.linkStartPoint);
        corners.push_back(edge.linkEndPoint);
        segmentStart = edge.linkEndPoint;
        currentLayerY = runtime.bakedCells[toCellIndex].elevationY;
        portals.clear();
    }

    const std::vector<glm::vec2> pulled = stringPull(
        glm::vec2(segmentStart.x, segmentStart.z),
        portals,
        glm::vec2(destination.x, destination.z)
    );
    for (std::size_t pointIndex = 1; pointIndex < pulled.size(); ++pointIndex) {
        corners.push_back(glm::vec3(pulled[pointIndex].x, currentLayerY, pulled[pointIndex].y));
    }
    if (corners.empty() || !nearlyEqualVec3(corners.back(), destination, agent->arrivalRadius)) {
        corners.push_back(destination);
    }
    if (!pathInsideWalkableSurfaceOrLinks(runtime, transform->position, corners, edgePath, agent->arrivalRadius)) {
        corners = buildConservativeCellPathCorners(
            runtime,
            transform->position,
            destination,
            cellPath,
            edgePath,
            agent->arrivalRadius
        );
        if (!pathInsideWalkableSurfaceOrLinks(runtime, transform->position, corners, edgePath, agent->arrivalRadius)) {
            return false;
        }
    }

    agent->pathCorners = std::move(corners);
    agent->destination = destination;
    agent->moving = !agent->pathCorners.empty();
    return true;
}

void NavigationSystem::updateAgents(World& world, const NavigationRuntime&, const TimeContext& time) const {
    for (EntityId entity : world.navAgents.entities()) {
        NavAgentComponent& agent = world.navAgents.get(entity);
        TransformComponent* transform = world.transforms.tryGet(entity);
        if (transform == nullptr) {
            continue;
        }

        AnimatedModelComponent* animation = world.animatedModels.tryGet(entity);
        LocomotionComponent* locomotion = world.locomotion.tryGet(entity);

        if (!agent.moving || agent.pathCorners.empty()) {
            agent.moving = false;
            if (animation != nullptr && locomotion != nullptr && locomotion->idleClip >= 0) {
                const int currentOrNext = animation->nextClip >= 0 ? animation->nextClip : animation->currentClip;
                if (currentOrNext != locomotion->idleClip) {
                    animation->requestedClip = locomotion->idleClip;
                }
            }
            continue;
        }

        const glm::vec3 target = agent.pathCorners.front();
        glm::vec3 toTarget = target - transform->position;
        const float distance = glm::length(toTarget);
        if (distance <= agent.arrivalRadius) {
            transform->position = target;
            agent.pathCorners.erase(agent.pathCorners.begin());
            world.markTransformsDirty(entity);
            if (agent.pathCorners.empty()) {
                agent.destination.reset();
                agent.moving = false;
            }
            continue;
        }

        const glm::vec3 direction = toTarget / std::max(distance, kPlaneEpsilon);
        const float step = agent.moveSpeed * time.deltaSeconds;
        if (step >= distance) {
            transform->position = target;
            agent.pathCorners.erase(agent.pathCorners.begin());
            if (agent.pathCorners.empty()) {
                agent.destination.reset();
                agent.moving = false;
            }
        } else {
            transform->position += direction * step;
        }

        const glm::vec2 planarDirection(direction.x, direction.z);
        if (glm::dot(planarDirection, planarDirection) > kPlaneEpsilon) {
            const float desiredYaw = glm::degrees(std::atan2(planarDirection.x, planarDirection.y));
            float updatedYaw = transform->rotationDeg.y;
            shortestYawStep(
                transform->rotationDeg.y,
                desiredYaw,
                agent.turnSpeedDeg * time.deltaSeconds,
                updatedYaw
            );
            transform->rotationDeg.y = updatedYaw;
        }

        world.markTransformsDirty(entity);
        if (animation != nullptr && locomotion != nullptr && locomotion->walkClip >= 0) {
            const int currentOrNext = animation->nextClip >= 0 ? animation->nextClip : animation->currentClip;
            if (currentOrNext != locomotion->walkClip) {
                animation->requestedClip = locomotion->walkClip;
            }
            animation->speed = 1.0f;
        }
    }
}

void NavigationSystem::syncFrame(const World& world, const NavigationRuntime& runtime, FrameSceneData& frame) const {
    frame.navigation.clear();
    frame.navigation.polygons.reserve(runtime.asset.polygons.size());
    frame.navigation.links.reserve(runtime.asset.links.size());
    for (const NavPolygon& polygon : runtime.asset.polygons) {
        FrameNavDebugPolygon debugPolygon{};
        debugPolygon.id = polygon.id;
        debugPolygon.elevationY = polygon.elevationY;
        debugPolygon.color = kWalkableOverlayColor;
        debugPolygon.vertices.reserve(polygon.verticesXZ.size());
        for (const glm::vec2& vertex : polygon.verticesXZ) {
            debugPolygon.vertices.push_back(glm::vec3(vertex.x, polygon.elevationY, vertex.y));
        }
        frame.navigation.polygons.push_back(std::move(debugPolygon));
    }
    for (const NavLink& link : runtime.asset.links) {
        frame.navigation.links.push_back(FrameNavDebugLink{
            link.id,
            link.fromPoint,
            link.toPoint,
            link.bidirectional
        });
    }
    for (EntityId entity : world.navAgents.entities()) {
        const NavAgentComponent& agent = world.navAgents.get(entity);
        frame.navigation.path = agent.pathCorners;
        frame.navigation.destination = agent.destination;
        break;
    }
    if (runtime.editor.polygonCaptureActive) {
        frame.navigation.captureElevationY = runtime.editor.polygonCaptureElevation;
        for (const glm::vec2& vertex : runtime.editor.polygonCaptureVertices) {
            frame.navigation.captureVertices.push_back(glm::vec3(vertex.x, runtime.editor.polygonCaptureElevation, vertex.y));
        }
    }
}

std::vector<EntityId> NavigationSystem::collectRenderableSelectionTargets(const World& world, EntityId selected) const {
    std::vector<EntityId> targets{};
    if (!selected.valid() || !world.isAlive(selected)) {
        return targets;
    }

    if (world.navSources.contains(selected)) {
        targets.push_back(selected);
        return targets;
    }

    const auto visit = [&](const auto& self, EntityId entity) -> void {
        for (EntityId child : world.parents.entities()) {
            const ParentComponent* parent = world.parents.tryGet(child);
            if (parent == nullptr || parent->parent != entity) {
                continue;
            }
            if (world.navSources.contains(child)) {
                targets.push_back(child);
            }
            self(self, child);
        }
    };
    visit(visit, selected);
    return targets;
}

void NavigationSystem::applyTagOverride(World& world, NavigationRuntime& runtime, EntityId entity, NavSourceTag tag) const {
    NavSourceComponent* source = world.navSources.tryGet(entity);
    if (source == nullptr) {
        return;
    }
    source->effectiveTag = tag;
    updateSourceOverride(runtime.asset.sourceTagOverrides, *source, tag);
}

bool NavigationSystem::capturePolygonClick(
    NavigationRuntime& runtime,
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight,
    int mouseX,
    int mouseY
) const {
    const std::optional<ViewportRay> ray = makeViewportRay(camera, viewportWidth, viewportHeight, mouseX, mouseY);
    if (!ray.has_value() || std::abs(ray->direction.y) <= kPlaneEpsilon) {
        return false;
    }

    const float t = (runtime.editor.polygonCaptureElevation - ray->origin.y) / ray->direction.y;
    if (t < 0.0f) {
        return false;
    }
    const glm::vec3 worldPoint = ray->origin + ray->direction * t;
    const glm::vec2 pointXZ(worldPoint.x, worldPoint.z);
    if (!runtime.editor.polygonCaptureVertices.empty() &&
        nearlyEqualVec2(runtime.editor.polygonCaptureVertices.back(), pointXZ, 0.01f)) {
        return false;
    }
    runtime.editor.polygonCaptureVertices.push_back(pointXZ);
    return true;
}

bool NavigationSystem::commitCapturedPolygon(NavigationRuntime& runtime, std::string* error) const {
    if (runtime.editor.polygonCaptureVertices.size() < 3u) {
        if (error) {
            *error = "Need at least three captured vertices.";
        }
        return false;
    }

    runtime.asset.polygons.push_back(NavPolygon{
        nextPolygonId(runtime.asset),
        runtime.editor.polygonCaptureElevation,
        runtime.editor.polygonCaptureVertices
    });
    runtime.editor.polygonCaptureVertices.clear();
    runtime.editor.polygonCaptureActive = false;
    return rebuildRuntime(runtime, error);
}

void NavigationSystem::clearCapturedPolygon(NavigationRuntime& runtime) const {
    runtime.editor.polygonCaptureVertices.clear();
    runtime.editor.polygonCaptureActive = false;
}

bool NavigationSystem::seedPendingLink(NavigationRuntime& runtime, int fromPolygonId, int toPolygonId, std::string* error) const {
    const auto fromIndex = findPolygonIndexById(runtime, fromPolygonId);
    const auto toIndex = findPolygonIndexById(runtime, toPolygonId);
    if (!fromIndex.has_value() || !toIndex.has_value()) {
        if (error) {
            *error = "Select valid polygons before seeding a link.";
        }
        return false;
    }

    const NavPolygon& fromPolygon = runtime.asset.polygons[*fromIndex];
    const NavPolygon& toPolygon = runtime.asset.polygons[*toIndex];
    float bestDistance = std::numeric_limits<float>::max();
    glm::vec2 bestFrom = fromPolygon.verticesXZ.front();
    glm::vec2 bestTo = toPolygon.verticesXZ.front();
    for (const glm::vec2& fromVertex : fromPolygon.verticesXZ) {
        for (const glm::vec2& toVertex : toPolygon.verticesXZ) {
            const float distance = glm::distance(fromVertex, toVertex);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestFrom = fromVertex;
                bestTo = toVertex;
            }
        }
    }

    runtime.editor.pendingLinkFromPolygonId = fromPolygonId;
    runtime.editor.pendingLinkToPolygonId = toPolygonId;
    runtime.editor.pendingLinkFromPoint = glm::vec3(bestFrom.x, fromPolygon.elevationY, bestFrom.y);
    runtime.editor.pendingLinkToPoint = glm::vec3(bestTo.x, toPolygon.elevationY, bestTo.y);
    runtime.editor.pendingLinkBidirectional = true;
    return true;
}

bool NavigationSystem::commitPendingLink(NavigationRuntime& runtime, std::string* error) const {
    if (runtime.editor.pendingLinkFromPolygonId < 0 || runtime.editor.pendingLinkToPolygonId < 0) {
        if (error) {
            *error = "Select source and target polygons first.";
        }
        return false;
    }

    runtime.asset.links.push_back(NavLink{
        nextLinkId(runtime.asset),
        runtime.editor.pendingLinkFromPolygonId,
        runtime.editor.pendingLinkToPolygonId,
        runtime.editor.pendingLinkFromPoint,
        runtime.editor.pendingLinkToPoint,
        runtime.editor.pendingLinkBidirectional
    });
    runtime.editor.pendingLinkFromPolygonId = -1;
    runtime.editor.pendingLinkToPolygonId = -1;
    runtime.editor.pendingLinkFromPoint = glm::vec3(0.0f);
    runtime.editor.pendingLinkToPoint = glm::vec3(0.0f);
    runtime.editor.pendingLinkBidirectional = true;
    return rebuildRuntime(runtime, error);
}

}  // namespace core
