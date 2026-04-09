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
 * Loads a GLB mesh, applies default-pose skinning when present, and recenters it
 * on the scene origin while keeping the lowest point on the ground plane.
 * @param path Path to the GLB asset.
 * @param outMesh Receives interleaved position/normal/color vertices and indices.
 * @return true when the model data is loaded successfully.
 */
bool loadStaticGltfModel(const std::string& path, StaticMeshData& outMesh);

/**
 * Backward-compatible wrapper for the original character-loading entry point.
 * @param path Path to the GLB asset.
 * @param outMesh Receives interleaved position/normal/color vertices and indices.
 * @return true when the model data is loaded successfully.
 */
bool loadStaticCharacterModel(const std::string& path, StaticMeshData& outMesh);

}  // namespace render
