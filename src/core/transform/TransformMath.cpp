#include "TransformMath.hpp"

#include <array>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

namespace core {

glm::mat4 composeTransform(const TransformComponent& transform) {
    glm::mat4 model(1.0f);
    model = glm::translate(model, transform.position);
    model = glm::rotate(model, glm::radians(transform.rotationDeg.x), glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, glm::radians(transform.rotationDeg.y), glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, glm::radians(transform.rotationDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));
    model = glm::scale(model, transform.scale);
    return model;
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

}  // namespace core
