#include "SceneModelFactory.hpp"

#include <algorithm>

#include <SDL.h>
#include <glm/gtc/quaternion.hpp>

namespace core::scene_detail {
namespace {

render::Bounds3 computeModelBounds(const render::GltfModelData& model) {
    render::Bounds3 bounds{};
    bool hasBounds = false;
    for (const auto& section : model.sections) {
        if (section.mesh.positions.empty()) {
            continue;
        }
        glm::mat4 nodeMatrix(1.0f);
        if (section.nodeIndex >= 0 && section.nodeIndex < static_cast<int>(model.nodes.size())) {
            nodeMatrix = model.nodes[static_cast<std::size_t>(section.nodeIndex)].bindGlobalMatrix;
        }
        render::Bounds3 sectionBounds{};
        bool hasSectionBounds = false;
        for (const glm::vec3& position : section.mesh.positions) {
            const glm::vec3 worldPosition = glm::vec3(nodeMatrix * glm::vec4(position, 1.0f));
            if (!hasSectionBounds) {
                sectionBounds = render::Bounds3{worldPosition, worldPosition};
                hasSectionBounds = true;
                continue;
            }
            sectionBounds.min = glm::min(sectionBounds.min, worldPosition);
            sectionBounds.max = glm::max(sectionBounds.max, worldPosition);
        }
        if (!hasSectionBounds) {
            continue;
        }
        if (!hasBounds) {
            bounds = sectionBounds;
            hasBounds = true;
            continue;
        }
        bounds.min = glm::min(bounds.min, sectionBounds.min);
        bounds.max = glm::max(bounds.max, sectionBounds.max);
    }
    return bounds;
}

}  // namespace

TransformComponent transformFromNodeTransform(const render::NodeTransform& transform) {
    return TransformComponent{
        transform.translation,
        glm::degrees(glm::eulerAngles(transform.rotation)),
        transform.scale
    };
}

bool fitModelToFootprint(render::GltfModelData& model, float targetFootprint) {
    const render::Bounds3 bounds = computeModelBounds(model);
    const glm::vec3 size = bounds.max - bounds.min;
    const float footprint = std::max(size.x, size.z);
    if (footprint <= 1.0e-4f) {
        return false;
    }
    const float scale = targetFootprint / footprint;
    for (int rootNodeIndex : model.sceneRootNodes) {
        if (rootNodeIndex >= 0 && rootNodeIndex < static_cast<int>(model.nodes.size())) {
            model.nodes[static_cast<std::size_t>(rootNodeIndex)].localTransform.scale *= glm::vec3(scale);
        }
    }
    render::refreshModelBindPose(model);
    return true;
}

std::string assetRootPath(const char* subdirectory) {
    char* basePath = SDL_GetBasePath();
    std::string root = basePath ? basePath : "";
    if (basePath != nullptr) {
        SDL_free(basePath);
    }
    return root + subdirectory;
}

}  // namespace core::scene_detail
