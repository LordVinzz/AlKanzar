#pragma once

#include <string>

#include "core/ecs/Components.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace core::scene_detail {

[[nodiscard]] TransformComponent transformFromNodeTransform(const render::NodeTransform& transform);
[[nodiscard]] bool fitModelToFootprint(render::GltfModelData& model, float targetFootprint);
[[nodiscard]] std::string assetRootPath(const char* subdirectory);

}  // namespace core::scene_detail
