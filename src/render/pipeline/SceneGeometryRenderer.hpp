#pragma once

#include <glm/mat3x3.hpp>

#include "render/engine/RenderResourceRegistry.hpp"
#include "render/engine/RenderSceneView.hpp"
#include "render/resources/ShaderProgram.hpp"

namespace render {

struct MaterialUniformLocations {
    GLint baseColorFactor{-1};
    GLint metallicFactor{-1};
    GLint roughnessFactor{-1};
    GLint normalScale{-1};
    GLint aoStrength{-1};
    GLint emissiveFactor{-1};
    GLint emissiveStrength{-1};
    GLint alphaFactor{-1};
    GLint alphaMode{-1};
    GLint alphaCutoff{-1};
    GLint clearcoatFactor{-1};
    GLint clearcoatRoughness{-1};
    GLint detailNormalScale{-1};
    GLint heightScale{-1};
    GLint baseColorUvSet{-1};
    GLint metallicRoughnessUvSet{-1};
    GLint normalUvSet{-1};
    GLint aoUvSet{-1};
    GLint emissiveUvSet{-1};
    GLint alphaUvSet{-1};
    GLint clearcoatUvSet{-1};
    GLint detailNormalUvSet{-1};
    GLint heightUvSet{-1};
    GLint baseColorUvTransform{-1};
    GLint metallicRoughnessUvTransform{-1};
    GLint normalUvTransform{-1};
    GLint aoUvTransform{-1};
    GLint emissiveUvTransform{-1};
    GLint alphaUvTransform{-1};
    GLint clearcoatUvTransform{-1};
    GLint detailNormalUvTransform{-1};
    GLint heightUvTransform{-1};
};

struct SceneGeometryShaderContext {
    GLint modelLocation{-1};
    GLint normalMatrixLocation{-1};
    GLint skinnedLocation{-1};
    GLint jointBaseIndexLocation{-1};
    GLint jointCountLocation{-1};
    MaterialUniformLocations materialLocations{};
};

class MaterialBinder {
public:
    [[nodiscard]] MaterialUniformLocations captureUniformLocations(const ShaderProgram& shader) const;
    [[nodiscard]] ShaderInputs resolveInputs(const Material& material, const RenderResourceRegistry& resources) const;
    void bindMaterialUniforms(const ShaderInputs& inputs, const MaterialUniformLocations& locations) const;
    void bindTextures(const ShaderInputs& inputs, const RenderResourceRegistry& resources) const;
    void prebindDefaults(const RenderResourceRegistry& resources) const;
    void configureRasterState(const ShaderInputs& inputs) const;
};

class SceneGeometryRenderer {
public:
    void drawLayer(
        const RenderSceneView& scene,
        RenderLayer layer,
        const SceneGeometryShaderContext& shaderContext,
        const MaterialBinder& materialBinder,
        const RenderResourceRegistry& resources,
        GLuint jointTextureBuffer
    ) const;
};

}  // namespace render
