#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"

#include <algorithm>
#include <cmath>

namespace core::navigation_detail {

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

glm::vec2 closestPointOnSegmentXZ(
    const glm::vec2& point,
    const glm::vec2& segmentStart,
    const glm::vec2& segmentEnd
) {
    const glm::vec2 segment = segmentEnd - segmentStart;
    const float segmentLengthSquared = glm::dot(segment, segment);
    const float segmentT = segmentLengthSquared > kPlaneEpsilon
        ? std::clamp(
            glm::dot(point - segmentStart, segment) /
                segmentLengthSquared,
            0.0f,
            1.0f
        )
        : 0.0f;
    return segmentStart + segment * segmentT;
}

glm::vec2 closestPointOnPolygonXZ(
    const glm::vec2& point,
    const std::vector<glm::vec2>& polygon
) {
    if (polygon.empty() || pointInOrOnPolygonXZ(point, polygon)) {
        return point;
    }

    glm::vec2 closest = polygon.front();
    float closestDistanceSquared =
        glm::dot(point - closest, point - closest);
    for (std::size_t index = 0u; index < polygon.size(); ++index) {
        const glm::vec2& edgeStart = polygon[index];
        const glm::vec2& edgeEnd = polygon[(index + 1u) % polygon.size()];
        const glm::vec2 candidate =
            closestPointOnSegmentXZ(point, edgeStart, edgeEnd);
        const float distanceSquared =
            glm::dot(point - candidate, point - candidate);
        if (distanceSquared < closestDistanceSquared) {
            closest = candidate;
            closestDistanceSquared = distanceSquared;
        }
    }
    return closest;
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


}  // namespace core::navigation_detail
