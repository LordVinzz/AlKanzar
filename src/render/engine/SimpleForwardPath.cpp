#include "RenderPaths.hpp"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "core/profiling/ProfilerService.hpp"

namespace render {

bool SimpleForwardPath::init(const std::string& shaderRoot, MaterialBinder& materialBinder, ShadowSystem&) {
    if (!shader_.buildFromFiles(shaderRoot + "simple.vert", shaderRoot + "simple.frag")) {
        return false;
    }

    modelLocation_ = shader_.uniformLocation("uModel");
    viewLocation_ = shader_.uniformLocation("uView");
    projLocation_ = shader_.uniformLocation("uProj");
    normalMatrixLocation_ = shader_.uniformLocation("uNormalMatrix");
    lightDirLocation_ = shader_.uniformLocation("uLightDir");

    geometryContext_.modelLocation = modelLocation_;
    geometryContext_.normalMatrixLocation = normalMatrixLocation_;
    geometryContext_.skinnedLocation = shader_.uniformLocation("uSkinned");
    geometryContext_.jointBaseIndexLocation = shader_.uniformLocation("uJointBaseIndex");
    geometryContext_.jointCountLocation = shader_.uniformLocation("uJointCount");
    geometryContext_.materialLocations = materialBinder.captureUniformLocations(shader_);

    shader_.use();
    glUniform1i(shader_.uniformLocation("uBaseColorTexture"), 0);
    glUniform1i(shader_.uniformLocation("uMetallicRoughnessTexture"), 1);
    glUniform1i(shader_.uniformLocation("uNormalTexture"), 2);
    glUniform1i(shader_.uniformLocation("uAoTexture"), 3);
    glUniform1i(shader_.uniformLocation("uEmissiveTexture"), 4);
    glUniform1i(shader_.uniformLocation("uAlphaTexture"), 5);
    glUniform1i(shader_.uniformLocation("uClearcoatTexture"), 6);
    glUniform1i(shader_.uniformLocation("uDetailNormalTexture"), 7);
    glUniform1i(shader_.uniformLocation("uHeightTexture"), 8);
    glUniform1i(shader_.uniformLocation("uJointBuffer"), 9);
    return true;
}

void SimpleForwardPath::resize(int, int) {}

void SimpleForwardPath::prepareOcclusionQueryPass(const RenderPathContext& context) const {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, context.width, context.height);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
}

bool SimpleForwardPath::beginFrame(const RenderPathContext&) {
    return shader_.id() != 0;
}

void SimpleForwardPath::prepareTargets(const RenderPathContext& context) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, context.width, context.height);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void SimpleForwardPath::drawGeometry(const RenderPathContext& context) {
    if (context.profiler) {
        ALKANZAR_PROFILE_SCOPE(*context.profiler, "Forward Geometry Pass");
        ALKANZAR_PROFILE_GPU_SCOPE(*context.profiler, "Forward Geometry Pass");
        shader_.use();
        glUniformMatrix4fv(viewLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.view));
        glUniformMatrix4fv(projLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.projection));
        const glm::vec3 dirLightView = glm::normalize(glm::mat3(context.camera.view) * glm::normalize(context.directionalLightDirection));
        glUniform3fv(lightDirLocation_, 1, glm::value_ptr(dirLightView));
        drawStandardSceneLayers(context, geometryContext_);
        return;
    }

    shader_.use();
    glUniformMatrix4fv(viewLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.view));
    glUniformMatrix4fv(projLocation_, 1, GL_FALSE, glm::value_ptr(context.camera.projection));
    const glm::vec3 dirLightView = glm::normalize(glm::mat3(context.camera.view) * glm::normalize(context.directionalLightDirection));
    glUniform3fv(lightDirLocation_, 1, glm::value_ptr(dirLightView));
    drawStandardSceneLayers(context, geometryContext_);
}

void SimpleForwardPath::composeFrame(const RenderPathContext&) {}

std::vector<ResourceMemoryRecord> SimpleForwardPath::profilingResources() const {
    return {};
}

}  // namespace render

