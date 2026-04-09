#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Geometry.hpp"
#include "Material.hpp"

namespace render {

struct StaticMeshSection {
    std::string name;
    Mesh mesh;
    std::shared_ptr<Material> material;
};

struct StaticModelData {
    std::vector<StaticMeshSection> sections;
};

bool loadStaticGltfModel(const std::string& path, StaticModelData& outModel);
bool loadStaticCharacterModel(const std::string& path, StaticModelData& outModel);

}  // namespace render
