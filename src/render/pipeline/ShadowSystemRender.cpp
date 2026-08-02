#include "ShadowSystem.hpp"

#include <glm/gtc/type_ptr.hpp>

namespace render {

void ShadowSystem::renderDirectionalShadows(
    const std::vector<ShadowRenderable>& renderables,
    GLuint jointTextureBuffer
) const {
    if (dirShadowMap_ == 0 || dirShadowFbo_ == 0 || dirCascadeCount_ == 0) {
        return;
    }
    if (dirUpdateEvery_ > 1 && (frameIndex_ % dirUpdateEvery_) != 0) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, dirShadowFbo_);
    glViewport(0, 0, dirShadowResolution_, dirShadowResolution_);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);

    shadowDepthShader_.use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_BUFFER, jointTextureBuffer);
    glUniform1i(shadowModeLocation_, 0);
    for (int cascade = 0; cascade < dirCascadeCount_; ++cascade) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, dirShadowMap_, 0, cascade);
        glClear(GL_DEPTH_BUFFER_BIT);
        glUniformMatrix4fv(shadowViewProjLocation_, 1, GL_FALSE, glm::value_ptr(dirShadowViewProj_[cascade]));
        for (const ShadowRenderable& renderable : renderables) {
            if (!renderable.mesh || !renderable.mesh->valid()) {
                continue;
            }
            glUniformMatrix4fv(shadowModelLocation_, 1, GL_FALSE, glm::value_ptr(renderable.modelMatrix));
            glUniform1i(shadowSkinnedLocation_, renderable.skinned ? 1 : 0);
            glUniform1i(shadowJointBaseIndexLocation_, renderable.jointMatrixBase);
            glUniform1i(shadowJointCountLocation_, renderable.jointMatrixCount);
            renderable.mesh->draw();
        }
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ShadowSystem::renderSpotShadows(
    const std::vector<ShadowRenderable>& renderables,
    GLuint jointTextureBuffer
) const {
    if (spotShadowMap_ == 0 || spotShadowFbo_ == 0 || spotShadowCount_ == 0) {
        return;
    }
    if (spotUpdateEvery_ > 1 && (frameIndex_ % spotUpdateEvery_) != 0) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, spotShadowFbo_);
    glViewport(0, 0, spotShadowResolution_, spotShadowResolution_);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);

    shadowDepthShader_.use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_BUFFER, jointTextureBuffer);
    for (int index = 0; index < spotShadowCount_; ++index) {
        glUniform1i(shadowModeLocation_, 1);
        glUniform3fv(shadowLightPositionLocation_, 1, glm::value_ptr(spotShadowPositions_[index]));
        glUniform1f(shadowFarPlaneLocation_, spotShadowFarPlanes_[index]);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, spotShadowMap_, 0, index);
        glClear(GL_DEPTH_BUFFER_BIT);
        glUniformMatrix4fv(shadowViewProjLocation_, 1, GL_FALSE, glm::value_ptr(spotShadowViewProj_[index]));
        for (const ShadowRenderable& renderable : renderables) {
            if (!renderable.mesh || !renderable.mesh->valid()) {
                continue;
            }
            glUniformMatrix4fv(shadowModelLocation_, 1, GL_FALSE, glm::value_ptr(renderable.modelMatrix));
            glUniform1i(shadowSkinnedLocation_, renderable.skinned ? 1 : 0);
            glUniform1i(shadowJointBaseIndexLocation_, renderable.jointMatrixBase);
            glUniform1i(shadowJointCountLocation_, renderable.jointMatrixCount);
            renderable.mesh->draw();
        }
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ShadowSystem::renderPointShadows(
    const std::vector<ShadowRenderable>& renderables,
    GLuint jointTextureBuffer
) const {
    if (pointShadowMap_ == 0 || pointShadowFbo_ == 0 || pointShadowCount_ == 0) {
        return;
    }
    if (pointUpdateEvery_ > 1 && (frameIndex_ % pointUpdateEvery_) != 0) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, pointShadowFbo_);
    glViewport(0, 0, pointShadowResolution_, pointShadowResolution_);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);

    shadowDepthShader_.use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_BUFFER, jointTextureBuffer);
    for (int index = 0; index < pointShadowCount_; ++index) {
        glUniform1i(shadowModeLocation_, 1);
        glUniform3fv(shadowLightPositionLocation_, 1, glm::value_ptr(pointShadowPositions_[index]));
        glUniform1f(shadowFarPlaneLocation_, pointShadowFarPlanes_[index]);
        for (int face = 0; face < 6; ++face) {
            const int layer = index * 6 + face;
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, pointShadowMap_, 0, layer);
            glClear(GL_DEPTH_BUFFER_BIT);
            glUniformMatrix4fv(shadowViewProjLocation_, 1, GL_FALSE, glm::value_ptr(pointShadowViewProj_[index][face]));
            for (const ShadowRenderable& renderable : renderables) {
                if (!renderable.mesh || !renderable.mesh->valid()) {
                    continue;
                }
                glUniformMatrix4fv(shadowModelLocation_, 1, GL_FALSE, glm::value_ptr(renderable.modelMatrix));
                glUniform1i(shadowSkinnedLocation_, renderable.skinned ? 1 : 0);
                glUniform1i(shadowJointBaseIndexLocation_, renderable.jointMatrixBase);
                glUniform1i(shadowJointCountLocation_, renderable.jointMatrixCount);
                renderable.mesh->draw();
            }
        }
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

}  // namespace render
