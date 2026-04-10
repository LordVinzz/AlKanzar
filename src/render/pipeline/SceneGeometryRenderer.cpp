#include "SceneGeometryRenderer.hpp"

#include <glm/gtc/type_ptr.hpp>

#include "core/transform/TransformMath.hpp"

namespace render {

MaterialUniformLocations MaterialBinder::captureUniformLocations(const ShaderProgram& shader) const {
    MaterialUniformLocations locations{};
    locations.baseColorFactor = shader.uniformLocation("uBaseColorFactor");
    locations.metallicFactor = shader.uniformLocation("uMetallicFactor");
    locations.roughnessFactor = shader.uniformLocation("uRoughnessFactor");
    locations.normalScale = shader.uniformLocation("uNormalScale");
    locations.aoStrength = shader.uniformLocation("uAoStrength");
    locations.emissiveFactor = shader.uniformLocation("uEmissiveFactor");
    locations.emissiveStrength = shader.uniformLocation("uEmissiveStrength");
    locations.alphaFactor = shader.uniformLocation("uAlphaFactor");
    locations.alphaMode = shader.uniformLocation("uAlphaMode");
    locations.alphaCutoff = shader.uniformLocation("uAlphaCutoff");
    locations.clearcoatFactor = shader.uniformLocation("uClearcoatFactor");
    locations.clearcoatRoughness = shader.uniformLocation("uClearcoatRoughness");
    locations.detailNormalScale = shader.uniformLocation("uDetailNormalScale");
    locations.heightScale = shader.uniformLocation("uHeightScale");
    locations.baseColorUvSet = shader.uniformLocation("uBaseColorUvSet");
    locations.metallicRoughnessUvSet = shader.uniformLocation("uMetallicRoughnessUvSet");
    locations.normalUvSet = shader.uniformLocation("uNormalUvSet");
    locations.aoUvSet = shader.uniformLocation("uAoUvSet");
    locations.emissiveUvSet = shader.uniformLocation("uEmissiveUvSet");
    locations.alphaUvSet = shader.uniformLocation("uAlphaUvSet");
    locations.clearcoatUvSet = shader.uniformLocation("uClearcoatUvSet");
    locations.detailNormalUvSet = shader.uniformLocation("uDetailNormalUvSet");
    locations.heightUvSet = shader.uniformLocation("uHeightUvSet");
    locations.baseColorUvTransform = shader.uniformLocation("uBaseColorUvTransform");
    locations.metallicRoughnessUvTransform = shader.uniformLocation("uMetallicRoughnessUvTransform");
    locations.normalUvTransform = shader.uniformLocation("uNormalUvTransform");
    locations.aoUvTransform = shader.uniformLocation("uAoUvTransform");
    locations.emissiveUvTransform = shader.uniformLocation("uEmissiveUvTransform");
    locations.alphaUvTransform = shader.uniformLocation("uAlphaUvTransform");
    locations.clearcoatUvTransform = shader.uniformLocation("uClearcoatUvTransform");
    locations.detailNormalUvTransform = shader.uniformLocation("uDetailNormalUvTransform");
    locations.heightUvTransform = shader.uniformLocation("uHeightUvTransform");
    return locations;
}

ShaderInputs MaterialBinder::resolveInputs(const Material& material, const RenderResourceRegistry& resources) const {
    return resolveShaderInputs(
        material,
        resources.defaultTextureForSlot(MaterialTextureSlot::BaseColor),
        resources.defaultTextureForSlot(MaterialTextureSlot::Normal),
        resources.defaultTextureForSlot(MaterialTextureSlot::MetallicRoughness),
        resources.defaultTextureForSlot(MaterialTextureSlot::Ao),
        resources.defaultTextureForSlot(MaterialTextureSlot::Emissive),
        resources.defaultTextureForSlot(MaterialTextureSlot::Alpha),
        resources.defaultTextureForSlot(MaterialTextureSlot::Clearcoat),
        resources.defaultTextureForSlot(MaterialTextureSlot::DetailNormal),
        resources.defaultTextureForSlot(MaterialTextureSlot::Height)
    );
}

void MaterialBinder::bindMaterialUniforms(const ShaderInputs& inputs, const MaterialUniformLocations& locations) const {
    glUniform3fv(locations.baseColorFactor, 1, glm::value_ptr(inputs.baseColorFactor));
    glUniform1f(locations.metallicFactor, inputs.metallicFactor);
    glUniform1f(locations.roughnessFactor, inputs.roughnessFactor);
    glUniform1f(locations.normalScale, inputs.normalScale);
    glUniform1f(locations.aoStrength, inputs.aoStrength);
    glUniform3fv(locations.emissiveFactor, 1, glm::value_ptr(inputs.emissiveFactor));
    glUniform1f(locations.emissiveStrength, inputs.emissiveStrength);
    glUniform1f(locations.alphaFactor, inputs.alphaFactor);
    glUniform1i(locations.alphaMode, static_cast<int>(inputs.alphaMode));
    glUniform1f(locations.alphaCutoff, inputs.alphaCutoff);
    glUniform1f(locations.clearcoatFactor, inputs.clearcoatFactor);
    glUniform1f(locations.clearcoatRoughness, inputs.clearcoatRoughness);
    glUniform1f(locations.detailNormalScale, inputs.detailNormalScale);
    glUniform1f(locations.heightScale, inputs.heightScale);

    const auto bindTextureMeta = [&](const TextureRef& textureRef, GLint uvSetLocation, GLint transformLocation) {
        glUniform1i(uvSetLocation, textureRef.uvSet);
        const glm::mat3 uvTransform = uvTransformMatrix(textureRef.transform);
        glUniformMatrix3fv(transformLocation, 1, GL_FALSE, glm::value_ptr(uvTransform));
    };

    bindTextureMeta(inputs.baseColorTexture, locations.baseColorUvSet, locations.baseColorUvTransform);
    bindTextureMeta(inputs.metallicRoughnessTexture, locations.metallicRoughnessUvSet, locations.metallicRoughnessUvTransform);
    bindTextureMeta(inputs.normalTexture, locations.normalUvSet, locations.normalUvTransform);
    bindTextureMeta(inputs.aoTexture, locations.aoUvSet, locations.aoUvTransform);
    bindTextureMeta(inputs.emissiveTexture, locations.emissiveUvSet, locations.emissiveUvTransform);
    bindTextureMeta(inputs.alphaTexture, locations.alphaUvSet, locations.alphaUvTransform);
    bindTextureMeta(inputs.clearcoatTexture, locations.clearcoatUvSet, locations.clearcoatUvTransform);
    bindTextureMeta(inputs.detailNormalTexture, locations.detailNormalUvSet, locations.detailNormalUvTransform);
    bindTextureMeta(inputs.heightTexture, locations.heightUvSet, locations.heightUvTransform);
}

void MaterialBinder::bindTextures(const ShaderInputs& inputs, const RenderResourceRegistry& resources) const {
    resources.bindTextureRef(0, inputs.baseColorTexture);
    resources.bindTextureRef(1, inputs.metallicRoughnessTexture);
    resources.bindTextureRef(2, inputs.normalTexture);
    resources.bindTextureRef(3, inputs.aoTexture);
    resources.bindTextureRef(4, inputs.emissiveTexture);
    resources.bindTextureRef(5, inputs.alphaTexture);
    resources.bindTextureRef(6, inputs.clearcoatTexture);
    resources.bindTextureRef(7, inputs.detailNormalTexture);
    resources.bindTextureRef(8, inputs.heightTexture);
}

void MaterialBinder::prebindDefaults(const RenderResourceRegistry& resources) const {
    for (int unit = 0; unit <= 8; ++unit) {
        resources.bindTextureRef(unit, resources.defaultTextureForUnit(unit));
    }
}

void MaterialBinder::configureRasterState(const ShaderInputs& inputs) const {
    if (inputs.doubleSided) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }

    if (inputs.alphaMode == AlphaMode::Blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
}

void SceneGeometryRenderer::drawLayer(
    const RenderSceneView& scene,
    RenderLayer layer,
    const SceneGeometryShaderContext& shaderContext,
    const MaterialBinder& materialBinder,
    const RenderResourceRegistry& resources,
    GLuint jointTextureBuffer
) const {
    const GLboolean depthWrite = layer == RenderLayer::Ground ? GL_FALSE : GL_TRUE;
    glDepthMask(depthWrite);
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_BUFFER, jointTextureBuffer);

    static const Material defaultMaterial{};
    for (const RenderSceneObjectView& object : scene.objects) {
        if (object.layer != layer || !object.visible || object.mesh == nullptr || !object.mesh->valid()) {
            continue;
        }

        const ShaderInputs inputs = materialBinder.resolveInputs(object.material ? *object.material : defaultMaterial, resources);
        materialBinder.configureRasterState(inputs);

        glUniformMatrix4fv(shaderContext.modelLocation, 1, GL_FALSE, glm::value_ptr(object.modelMatrix));
        const glm::mat3 normalMatrix = core::normalMatrixFromModel(object.modelMatrix);
        glUniformMatrix3fv(shaderContext.normalMatrixLocation, 1, GL_FALSE, glm::value_ptr(normalMatrix));
        glUniform1i(shaderContext.skinnedLocation, object.skinned ? 1 : 0);
        glUniform1i(shaderContext.jointBaseIndexLocation, object.jointMatrixBase);
        glUniform1i(shaderContext.jointCountLocation, object.jointMatrixCount);
        materialBinder.bindMaterialUniforms(inputs, shaderContext.materialLocations);
        materialBinder.bindTextures(inputs, resources);
        object.mesh->draw();
    }

    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

}  // namespace render
