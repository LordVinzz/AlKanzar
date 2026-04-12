#include "PickingSystem.hpp"

#include <limits>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

namespace core {

namespace {

bool intersectRaySphere(const ViewportRay& ray, const glm::vec3& center, float radius, float& outT) {
    const glm::vec3 offset = ray.origin - center;
    const float a = glm::dot(ray.direction, ray.direction);
    const float b = 2.0f * glm::dot(offset, ray.direction);
    const float c = glm::dot(offset, offset) - radius * radius;
    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) {
        return false;
    }

    const float sqrtDisc = std::sqrt(discriminant);
    const float t0 = (-b - sqrtDisc) / (2.0f * a);
    const float t1 = (-b + sqrtDisc) / (2.0f * a);
    if (t0 >= 0.0f) {
        outT = t0;
        return true;
    }
    if (t1 >= 0.0f) {
        outT = t1;
        return true;
    }
    return false;
}

bool intersectRayAabb(const ViewportRay& ray, const render::Bounds3& bounds, float& outT) {
    float tMin = 0.0f;
    float tMax = std::numeric_limits<float>::max();

    for (int axis = 0; axis < 3; ++axis) {
        const float origin = ray.origin[axis];
        const float direction = ray.direction[axis];
        const float minValue = bounds.min[axis];
        const float maxValue = bounds.max[axis];

        if (std::abs(direction) < 1.0e-6f) {
            if (origin < minValue || origin > maxValue) {
                return false;
            }
            continue;
        }

        float t0 = (minValue - origin) / direction;
        float t1 = (maxValue - origin) / direction;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return false;
        }
    }

    outT = tMin;
    return true;
}

}  // namespace

std::optional<ViewportRay> makeViewportRay(
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight,
    int mouseX,
    int mouseY
) {
    if (viewportWidth <= 0 || viewportHeight <= 0) {
        return std::nullopt;
    }

    const float ndcX = (static_cast<float>(mouseX) / static_cast<float>(viewportWidth)) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (static_cast<float>(mouseY) / static_cast<float>(viewportHeight)) * 2.0f;
    const glm::vec4 nearClip(ndcX, ndcY, -1.0f, 1.0f);
    const glm::vec4 farClip(ndcX, ndcY, 1.0f, 1.0f);
    const glm::mat4 invView = glm::inverse(camera.view);
    const glm::vec4 nearView = camera.invProjection * nearClip;
    const glm::vec4 farView = camera.invProjection * farClip;
    const glm::vec3 nearWorld = glm::vec3(invView * (nearView / nearView.w));
    const glm::vec3 farWorld = glm::vec3(invView * (farView / farView.w));
    const glm::vec3 direction = farWorld - nearWorld;
    if (glm::dot(direction, direction) < 1.0e-6f) {
        return std::nullopt;
    }

    return ViewportRay{nearWorld, glm::normalize(direction)};
}

std::optional<EntityId> PickingSystem::pick(
    const FrameSceneData& frame,
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight,
    int mouseX,
    int mouseY,
    bool includeLights
) const {
    const std::optional<ViewportRay> ray = makeViewportRay(
        camera,
        viewportWidth,
        viewportHeight,
        mouseX,
        mouseY
    );
    if (!ray.has_value()) {
        return std::nullopt;
    }
    bool hit = false;
    float bestT = std::numeric_limits<float>::max();
    EntityId bestEntity{};

    for (const FrameRenderable& renderable : frame.renderables) {
        if (!renderable.visible || !renderable.entity.valid()) {
            continue;
        }

        float tHit = 0.0f;
        if (!intersectRayAabb(*ray, renderable.worldBounds, tHit)) {
            continue;
        }
        if (tHit < bestT) {
            bestT = tHit;
            bestEntity = renderable.entity;
            hit = true;
        }
    }

    if (includeLights) {
        for (const FrameLight& light : frame.lights) {
            glm::vec3 sphereCenter = light.position;
            float sphereRadius = light.radius;
            if (light.type == render::LightType::Spot) {
                const float coneRadius = light.radius * std::tan(glm::radians(light.outerAngle));
                sphereCenter = light.position + light.direction * (light.radius * 0.5f);
                sphereRadius = std::sqrt((light.radius * 0.5f) * (light.radius * 0.5f) + coneRadius * coneRadius);
            }

            float tHit = 0.0f;
            if (!intersectRaySphere(*ray, sphereCenter, sphereRadius, tHit)) {
                continue;
            }
            if (tHit < bestT) {
                bestT = tHit;
                bestEntity = light.entity;
                hit = true;
            }
        }
    }

    return hit ? std::optional<EntityId>(bestEntity) : std::nullopt;
}

}  // namespace core
