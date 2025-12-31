#include "ShadowSystem.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

namespace {

constexpr float kSpotNearPlane = 0.1f;
constexpr float kPointNearPlane = 0.1f;

std::array<glm::vec3, 8> getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view) {
    const glm::mat4 inv = glm::inverse(proj * view);
    std::array<glm::vec3, 8> corners{};
    int idx = 0;
    for (int z = 0; z < 2; ++z) {
        const float ndcZ = z == 0 ? -1.0f : 1.0f;
        for (int y = 0; y < 2; ++y) {
            const float ndcY = y == 0 ? 1.0f : -1.0f;
            for (int x = 0; x < 2; ++x) {
                const float ndcX = x == 0 ? -1.0f : 1.0f;
                const glm::vec4 corner = inv * glm::vec4(ndcX, ndcY, ndcZ, 1.0f);
                corners[idx++] = glm::vec3(corner) / corner.w;
            }
        }
    }
    return corners;
}

glm::vec3 stableUp(const glm::vec3& dir) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(dir, up)) > 0.95f) {
        return glm::vec3(0.0f, 0.0f, 1.0f);
    }
    return up;
}

}  // namespace

namespace render {

bool ShadowSystem::init(const std::string& shaderRoot) {
    dirCascadeCount_ = std::clamp(dirCascadeCount_, 1, kMaxCascades);
    dirUpdateEvery_ = std::max(dirUpdateEvery_, 1);
    spotUpdateEvery_ = std::max(spotUpdateEvery_, 1);
    pointUpdateEvery_ = std::max(pointUpdateEvery_, 1);
    const std::string shadowVertex = shaderRoot + "shadow_depth.vert";
    const std::string shadowFragment = shaderRoot + "shadow_depth.frag";
    if (!shadowDepthShader_.buildFromFiles(shadowVertex, shadowFragment)) {
        spdlog::error("ShadowSystem: failed to build shadow depth shader");
        return false;
    }
    shadowMvpLocation_ = shadowDepthShader_.uniformLocation("uLightMVP");

    ensureDirectionalResources();
    ensureSpotResources();
    ensurePointResources();
    return true;
}

void ShadowSystem::destroy() {
    destroyResources();
}

void ShadowSystem::destroyResources() {
    if (dirShadowFbo_ != 0) {
        glDeleteFramebuffers(1, &dirShadowFbo_);
        dirShadowFbo_ = 0;
    }
    if (dirShadowMap_ != 0) {
        glDeleteTextures(1, &dirShadowMap_);
        dirShadowMap_ = 0;
    }
    if (spotShadowFbo_ != 0) {
        glDeleteFramebuffers(1, &spotShadowFbo_);
        spotShadowFbo_ = 0;
    }
    if (spotShadowMap_ != 0) {
        glDeleteTextures(1, &spotShadowMap_);
        spotShadowMap_ = 0;
    }
    if (pointShadowFbo_ != 0) {
        glDeleteFramebuffers(1, &pointShadowFbo_);
        pointShadowFbo_ = 0;
    }
    if (pointShadowMap_ != 0) {
        glDeleteTextures(1, &pointShadowMap_);
        pointShadowMap_ = 0;
    }
}

void ShadowSystem::ensureDirectionalResources() {
    if (dirShadowMap_ != 0 && dirShadowFbo_ != 0) {
        return;
    }

    glGenTextures(1, &dirShadowMap_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, dirShadowMap_);
    glTexImage3D(
        GL_TEXTURE_2D_ARRAY,
        0,
        GL_DEPTH_COMPONENT24,
        dirShadowResolution_,
        dirShadowResolution_,
        dirCascadeCount_,
        0,
        GL_DEPTH_COMPONENT,
        GL_FLOAT,
        nullptr
    );
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    const float border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    glGenFramebuffers(1, &dirShadowFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, dirShadowFbo_);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    dirTexelSize_ = glm::vec2(1.0f / static_cast<float>(dirShadowResolution_));
}

void ShadowSystem::ensureSpotResources() {
    if (spotShadowMap_ != 0 && spotShadowFbo_ != 0) {
        return;
    }

    glGenTextures(1, &spotShadowMap_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, spotShadowMap_);
    glTexImage3D(
        GL_TEXTURE_2D_ARRAY,
        0,
        GL_DEPTH_COMPONENT24,
        spotShadowResolution_,
        spotShadowResolution_,
        kMaxSpotShadows,
        0,
        GL_DEPTH_COMPONENT,
        GL_FLOAT,
        nullptr
    );
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    const float border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    glGenFramebuffers(1, &spotShadowFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, spotShadowFbo_);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    spotTexelSize_ = glm::vec2(1.0f / static_cast<float>(spotShadowResolution_));
}

void ShadowSystem::ensurePointResources() {
    if (pointShadowMap_ != 0 && pointShadowFbo_ != 0) {
        return;
    }

    glGenTextures(1, &pointShadowMap_);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, pointShadowMap_);
    glTexImage3D(
        GL_TEXTURE_CUBE_MAP_ARRAY,
        0,
        GL_DEPTH_COMPONENT24,
        pointShadowResolution_,
        pointShadowResolution_,
        kMaxPointShadows * 6,
        0,
        GL_DEPTH_COMPONENT,
        GL_FLOAT,
        nullptr
    );
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);

    glGenFramebuffers(1, &pointShadowFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, pointShadowFbo_);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    pointShadowDiskRadius_ = 2.5f / static_cast<float>(pointShadowResolution_);
}

void ShadowSystem::updateDirectional(
    const glm::mat4& view,
    const glm::mat4& proj,
    const glm::vec3& lightDirWorld,
    float nearPlane,
    float farPlane
) {
    const glm::mat4 invView = glm::inverse(view);
    const std::array<glm::vec3, 8> corners = getFrustumCornersWorldSpace(proj, view);

    const float clipRange = farPlane - nearPlane;
    const float minZ = nearPlane;
    const float maxZ = farPlane;
    const float range = maxZ - minZ;
    const float ratio = maxZ / minZ;

    float prevSplitDist = 0.0f;

    for (int i = 0; i < dirCascadeCount_; ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(dirCascadeCount_);
        const float logSplit = minZ * std::pow(ratio, p);
        const float uniformSplit = minZ + range * p;
        const float split = dirSplitLambda_ * (logSplit - uniformSplit) + uniformSplit;
        const float splitDist = (split - nearPlane) / clipRange;

        std::array<glm::vec3, 8> cascadeCorners{};
        for (int c = 0; c < 4; ++c) {
            const glm::vec3 cornerNear = corners[c];
            const glm::vec3 cornerFar = corners[c + 4];
            cascadeCorners[c] = cornerNear + (cornerFar - cornerNear) * prevSplitDist;
            cascadeCorners[c + 4] = cornerNear + (cornerFar - cornerNear) * splitDist;
        }

        glm::vec3 center(0.0f);
        for (const auto& corner : cascadeCorners) {
            center += corner;
        }
        center /= static_cast<float>(cascadeCorners.size());

        const glm::vec3 lightDir = glm::normalize(lightDirWorld);
        const glm::vec3 up = stableUp(lightDir);
        const glm::mat4 lightView = glm::lookAt(center - lightDir * 50.0f, center, up);

        glm::vec3 minCorner(std::numeric_limits<float>::max());
        glm::vec3 maxCorner(std::numeric_limits<float>::lowest());
        for (const auto& corner : cascadeCorners) {
            const glm::vec3 lightSpace = glm::vec3(lightView * glm::vec4(corner, 1.0f));
            minCorner = glm::min(minCorner, lightSpace);
            maxCorner = glm::max(maxCorner, lightSpace);
        }

        minCorner.z -= dirZPadding_;
        maxCorner.z += dirZPadding_;

        glm::vec3 extent = (maxCorner - minCorner) * 0.5f;
        glm::vec3 centerLS = (maxCorner + minCorner) * 0.5f;
        const float texelX = (extent.x * 2.0f) / static_cast<float>(dirShadowResolution_);
        const float texelY = (extent.y * 2.0f) / static_cast<float>(dirShadowResolution_);
        centerLS.x = std::floor(centerLS.x / texelX) * texelX;
        centerLS.y = std::floor(centerLS.y / texelY) * texelY;
        minCorner.x = centerLS.x - extent.x;
        maxCorner.x = centerLS.x + extent.x;
        minCorner.y = centerLS.y - extent.y;
        maxCorner.y = centerLS.y + extent.y;

        const float nearClip = -maxCorner.z;
        const float farClip = -minCorner.z;
        const glm::mat4 lightProj = glm::ortho(minCorner.x, maxCorner.x, minCorner.y, maxCorner.y, nearClip, farClip);

        dirShadowViewProj_[i] = lightProj * lightView;
        dirShadowMatrices_[i] = dirShadowViewProj_[i] * invView;
        dirCascadeSplits_[i] = split;

        prevSplitDist = splitDist;
    }
}

void ShadowSystem::beginFrame() {
    spotShadowCount_ = 0;
    pointShadowCount_ = 0;
    frameIndex_++;
}

int ShadowSystem::registerSpotShadow(const SpotShadowDesc& desc, const glm::mat4& invView) {
    if (spotShadowCount_ >= kMaxSpotShadows) {
        return -1;
    }
    (void)desc.biasMin;
    (void)desc.biasSlope;
    const int idx = spotShadowCount_++;
    const glm::vec3 dir = glm::normalize(desc.direction);
    const glm::vec3 up = stableUp(dir);
    const float farPlane = std::max(desc.radius, 0.2f);
    const glm::mat4 lightView = glm::lookAt(desc.position, desc.position + dir, up);
    const glm::mat4 lightProj = glm::perspective(glm::radians(desc.outerAngleDeg * 2.0f), 1.0f, kSpotNearPlane, farPlane);

    spotShadowViewProj_[idx] = lightProj * lightView;
    spotShadowMatrices_[idx] = spotShadowViewProj_[idx] * invView;
    return idx;
}

int ShadowSystem::registerPointShadow(const PointShadowDesc& desc) {
    if (pointShadowCount_ >= kMaxPointShadows) {
        return -1;
    }
    (void)desc.biasMin;
    (void)desc.biasSlope;
    const int idx = pointShadowCount_++;
    const float farPlane = std::max(desc.radius, 0.2f);
    const glm::mat4 lightProj = glm::perspective(glm::radians(90.0f), 1.0f, kPointNearPlane, farPlane);

    const std::array<glm::vec3, 6> directions = {
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 0.0f, -1.0f)
    };
    const std::array<glm::vec3, 6> ups = {
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f)
    };

    for (int face = 0; face < 6; ++face) {
        const glm::mat4 lightView = glm::lookAt(desc.position, desc.position + directions[face], ups[face]);
        pointShadowViewProj_[idx][face] = lightProj * lightView;
    }

    return idx;
}

void ShadowSystem::renderDirectionalShadows(std::initializer_list<const MeshBuffer*> meshes) const {
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
    for (int cascade = 0; cascade < dirCascadeCount_; ++cascade) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, dirShadowMap_, 0, cascade);
        glClear(GL_DEPTH_BUFFER_BIT);
        glUniformMatrix4fv(shadowMvpLocation_, 1, GL_FALSE, glm::value_ptr(dirShadowViewProj_[cascade]));
        for (const MeshBuffer* mesh : meshes) {
            mesh->draw();
        }
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ShadowSystem::renderSpotShadows(std::initializer_list<const MeshBuffer*> meshes) const {
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
    for (int i = 0; i < spotShadowCount_; ++i) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, spotShadowMap_, 0, i);
        glClear(GL_DEPTH_BUFFER_BIT);
        glUniformMatrix4fv(shadowMvpLocation_, 1, GL_FALSE, glm::value_ptr(spotShadowViewProj_[i]));
        for (const MeshBuffer* mesh : meshes) {
            mesh->draw();
        }
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ShadowSystem::renderPointShadows(std::initializer_list<const MeshBuffer*> meshes) const {
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
    for (int i = 0; i < pointShadowCount_; ++i) {
        for (int face = 0; face < 6; ++face) {
            const int layer = i * 6 + face;
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, pointShadowMap_, 0, layer);
            glClear(GL_DEPTH_BUFFER_BIT);
            glUniformMatrix4fv(shadowMvpLocation_, 1, GL_FALSE, glm::value_ptr(pointShadowViewProj_[i][face]));
            for (const MeshBuffer* mesh : meshes) {
                mesh->draw();
            }
        }
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

}  // namespace render
