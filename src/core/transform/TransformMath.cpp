#include "TransformMath.hpp"

#include <array>
#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace core {

namespace {

glm::vec3 axisFromColumn(const glm::mat4& matrix, int columnIndex) {
    return glm::vec3(matrix[columnIndex]);
}

float axisLength(const glm::vec3& axis) {
    return glm::max(glm::length(axis), 1.0e-6f);
}

}  // namespace

glm::mat4 composeTransform(const TransformComponent& transform) {
    glm::mat4 model(1.0f);
    model = glm::translate(model, transform.position);
    model = glm::rotate(model, glm::radians(transform.rotationDeg.x), glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, glm::radians(transform.rotationDeg.y), glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, glm::radians(transform.rotationDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));
    model = glm::scale(model, transform.scale);
    return model;
}

bool decomposeTransform(const glm::mat4& matrix, TransformComponent& outTransform) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }

    glm::vec3 scale{};
    glm::quat rotation{};
    glm::vec3 translation{};
    glm::vec3 skew{};
    glm::vec4 perspective{};
    if (!glm::decompose(matrix, scale, rotation, translation, skew, perspective) ||
        glm::length(skew) > 1.0e-4f ||
        glm::length(glm::vec3(perspective)) > 1.0e-4f ||
        glm::abs(perspective.w - 1.0f) > 1.0e-4f ||
        glm::any(glm::lessThanEqual(glm::abs(scale), glm::vec3(1.0e-5f)))) {
        return false;
    }

    glm::mat4 rotationMatrix(1.0f);
    for (int column = 0; column < 3; ++column) {
        rotationMatrix[column] = glm::vec4(glm::vec3(matrix[column]) / scale[column], 0.0f);
    }
    float rotationX = 0.0f;
    float rotationY = 0.0f;
    float rotationZ = 0.0f;
    glm::extractEulerAngleXYZ(rotationMatrix, rotationX, rotationY, rotationZ);
    outTransform = TransformComponent{
        translation,
        glm::degrees(glm::vec3(rotationX, rotationY, rotationZ)),
        scale
    };
    return true;
}

glm::mat3 normalMatrixFromModel(const glm::mat4& model) {
    return glm::mat3(glm::transpose(glm::inverse(model)));
}

render::Bounds3 transformBounds(const render::Bounds3& bounds, const glm::mat4& model) {
    std::array<glm::vec3, 8> corners = {{
        {bounds.min.x, bounds.min.y, bounds.min.z},
        {bounds.min.x, bounds.min.y, bounds.max.z},
        {bounds.min.x, bounds.max.y, bounds.min.z},
        {bounds.min.x, bounds.max.y, bounds.max.z},
        {bounds.max.x, bounds.min.y, bounds.min.z},
        {bounds.max.x, bounds.min.y, bounds.max.z},
        {bounds.max.x, bounds.max.y, bounds.min.z},
        {bounds.max.x, bounds.max.y, bounds.max.z},
    }};

    render::Bounds3 transformed{};
    transformed.min = glm::vec3(model * glm::vec4(corners.front(), 1.0f));
    transformed.max = transformed.min;

    for (const glm::vec3& corner : corners) {
        const glm::vec3 transformedCorner = glm::vec3(model * glm::vec4(corner, 1.0f));
        transformed.min = glm::min(transformed.min, transformedCorner);
        transformed.max = glm::max(transformed.max, transformedCorner);
    }

    return transformed;
}

OrientedBox makeOrientedBox(const glm::mat4& modelMatrix, const BoxColliderComponent& collider) {
    OrientedBox box{};
    const glm::vec3 scaledAxisX = axisFromColumn(modelMatrix, 0);
    const glm::vec3 scaledAxisY = axisFromColumn(modelMatrix, 1);
    const glm::vec3 scaledAxisZ = axisFromColumn(modelMatrix, 2);

    const float scaleX = axisLength(scaledAxisX);
    const float scaleY = axisLength(scaledAxisY);
    const float scaleZ = axisLength(scaledAxisZ);

    if (collider.rotatesWithEntity) {
        box.center = glm::vec3(
            modelMatrix * glm::vec4(collider.center, 1.0f)
        );
        box.axes[0] = scaledAxisX / scaleX;
        box.axes[1] = scaledAxisY / scaleY;
        box.axes[2] = scaledAxisZ / scaleZ;
    } else {
        box.center = glm::vec3(modelMatrix[3]) +
            collider.center * glm::vec3(scaleX, scaleY, scaleZ);
        box.axes[0] = glm::vec3(1.0f, 0.0f, 0.0f);
        box.axes[1] = glm::vec3(0.0f, 1.0f, 0.0f);
        box.axes[2] = glm::vec3(0.0f, 0.0f, 1.0f);
    }
    box.halfExtents = glm::max(glm::vec3(
        collider.halfExtents.x * scaleX,
        collider.halfExtents.y * scaleY,
        collider.halfExtents.z * scaleZ
    ), glm::vec3(0.001f));

    box.modelMatrix = glm::mat4(
        glm::vec4(box.axes[0] * box.halfExtents.x, 0.0f),
        glm::vec4(box.axes[1] * box.halfExtents.y, 0.0f),
        glm::vec4(box.axes[2] * box.halfExtents.z, 0.0f),
        glm::vec4(box.center, 1.0f)
    );

    std::size_t cornerIndex = 0u;
    for (float signX : {-1.0f, 1.0f}) {
        for (float signY : {-1.0f, 1.0f}) {
            for (float signZ : {-1.0f, 1.0f}) {
                box.corners[cornerIndex++] = box.center +
                    box.axes[0] * (box.halfExtents.x * signX) +
                    box.axes[1] * (box.halfExtents.y * signY) +
                    box.axes[2] * (box.halfExtents.z * signZ);
            }
        }
    }

    box.aabb.min = box.corners.front();
    box.aabb.max = box.corners.front();
    for (const glm::vec3& corner : box.corners) {
        box.aabb.min = glm::min(box.aabb.min, corner);
        box.aabb.max = glm::max(box.aabb.max, corner);
    }
    return box;
}

OrientedBox makeOrientedBox(const TransformComponent& transform, const BoxColliderComponent& collider) {
    return makeOrientedBox(composeTransform(transform), collider);
}

}  // namespace core
