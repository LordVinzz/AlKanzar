#pragma once

#include <string>
#include <vector>

namespace render {

/**
 * Flattened static mesh data ready for MeshBuffer upload.
 */
struct StaticMeshData {
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
};

/**
 * Loads a GLB character, applies default-pose skinning, and recenters it on the
 * scene origin while keeping the feet on the ground plane.
 * @param path Path to the GLB asset.
 * @param outMesh Receives interleaved position/normal/color vertices and indices.
 * @return true when the character data is loaded successfully.
 */
bool loadStaticCharacterModel(const std::string& path, StaticMeshData& outMesh);

}  // namespace render
