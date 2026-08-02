#include "SceneOverlayMath.hpp"

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace render::detail {
namespace {

glm::vec3 stableUp(const glm::vec3& direction) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    return std::abs(glm::dot(direction, up)) > 0.95f
        ? glm::vec3(0.0f, 0.0f, 1.0f)
        : up;
}

}  // namespace

glm::vec3 normalizeOr(const glm::vec3& value, const glm::vec3& fallback) {
    const float length = glm::length(value);
    return length <= 1.0e-6f ? fallback : value / length;
}

glm::mat4 makeOrientationFromDirection(const glm::vec3& direction) {
    if (glm::dot(direction, direction) < 1.0e-6f) {
        return glm::mat4(1.0f);
    }
    const glm::vec3 forward = glm::normalize(direction);
    const glm::vec3 right = glm::normalize(glm::cross(stableUp(forward), forward));
    const glm::vec3 actualUp = glm::normalize(glm::cross(forward, right));
    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(right, 0.0f);
    basis[1] = glm::vec4(actualUp, 0.0f);
    basis[2] = glm::vec4(forward, 0.0f);
    return basis;
}

glm::mat4 makeUnscaledSelectionAxisModel(
    const glm::mat4& transformMatrix,
    float axisScale
) {
    const glm::vec3 originalX(transformMatrix[0]);
    const glm::vec3 originalY(transformMatrix[1]);
    const glm::vec3 originalZ(transformMatrix[2]);
    const glm::vec3 xAxis = normalizeOr(originalX, glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::vec3 yAxis = normalizeOr(
        originalY - xAxis * glm::dot(originalY, xAxis),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    glm::vec3 zAxis = glm::cross(xAxis, yAxis);
    zAxis = glm::dot(zAxis, zAxis) <= 1.0e-6f
        ? normalizeOr(originalZ, glm::vec3(0.0f, 0.0f, 1.0f))
        : glm::normalize(zAxis);
    if (glm::dot(zAxis, originalZ) < 0.0f) {
        zAxis = -zAxis;
    }
    const glm::vec3 correctedY = normalizeOr(
        glm::cross(zAxis, xAxis),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    glm::mat4 model(1.0f);
    model[0] = glm::vec4(xAxis, 0.0f);
    model[1] = glm::vec4(correctedY, 0.0f);
    model[2] = glm::vec4(zAxis, 0.0f);
    model[3] = glm::vec4(glm::vec3(transformMatrix[3]), 1.0f);
    return glm::scale(model, glm::vec3(axisScale));
}

}  // namespace render::detail
