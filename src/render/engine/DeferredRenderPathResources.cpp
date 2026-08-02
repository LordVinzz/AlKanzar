#include "RenderPaths.hpp"

namespace render {

void DeferredRenderPath::destroyResources() {
    if (gbufferFbo_ != 0) glDeleteFramebuffers(1, &gbufferFbo_);
    if (lightFbo_ != 0) glDeleteFramebuffers(1, &lightFbo_);
    if (gbufferAlbedo_ != 0) glDeleteTextures(1, &gbufferAlbedo_);
    if (gbufferNormal_ != 0) glDeleteTextures(1, &gbufferNormal_);
    if (gbufferEmissiveAo_ != 0) glDeleteTextures(1, &gbufferEmissiveAo_);
    if (gbufferClearcoat_ != 0) glDeleteTextures(1, &gbufferClearcoat_);
    if (gbufferDepthColor_ != 0) glDeleteTextures(1, &gbufferDepthColor_);
    if (gbufferDepth_ != 0) glDeleteTextures(1, &gbufferDepth_);
    if (lightColor_ != 0) glDeleteTextures(1, &lightColor_);
    if (lightBuffers_.texture != 0) glDeleteTextures(1, &lightBuffers_.texture);
    if (lightBuffers_.buffer != 0) glDeleteBuffers(1, &lightBuffers_.buffer);
    if (fullscreenVao_ != 0) glDeleteVertexArrays(1, &fullscreenVao_);

    gbufferFbo_ = 0;
    lightFbo_ = 0;
    gbufferAlbedo_ = 0;
    gbufferNormal_ = 0;
    gbufferEmissiveAo_ = 0;
    gbufferClearcoat_ = 0;
    gbufferDepthColor_ = 0;
    gbufferDepth_ = 0;
    lightColor_ = 0;
    fullscreenVao_ = 0;
    deferredWidth_ = 0;
    deferredHeight_ = 0;
    lightBuffers_ = {};
}

void DeferredRenderPath::ensureResources(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    if (width == deferredWidth_ && height == deferredHeight_ && gbufferFbo_ != 0 && lightFbo_ != 0) {
        return;
    }

    destroyResources();
    deferredWidth_ = width;
    deferredHeight_ = height;

    glGenTextures(1, &gbufferAlbedo_);
    glBindTexture(GL_TEXTURE_2D, gbufferAlbedo_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferNormal_);
    glBindTexture(GL_TEXTURE_2D, gbufferNormal_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferEmissiveAo_);
    glBindTexture(GL_TEXTURE_2D, gbufferEmissiveAo_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferClearcoat_);
    glBindTexture(GL_TEXTURE_2D, gbufferClearcoat_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferDepthColor_);
    glBindTexture(GL_TEXTURE_2D, gbufferDepthColor_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width, height, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &gbufferDepth_);
    glBindTexture(GL_TEXTURE_2D, gbufferDepth_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);

    glGenFramebuffers(1, &gbufferFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, gbufferFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gbufferAlbedo_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gbufferNormal_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, gbufferEmissiveAo_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, gbufferClearcoat_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4, GL_TEXTURE_2D, gbufferDepthColor_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, gbufferDepth_, 0);
    const GLenum gbufferAttachments[5] = {
        GL_COLOR_ATTACHMENT0,
        GL_COLOR_ATTACHMENT1,
        GL_COLOR_ATTACHMENT2,
        GL_COLOR_ATTACHMENT3,
        GL_COLOR_ATTACHMENT4,
    };
    glDrawBuffers(5, gbufferAttachments);

    glGenTextures(1, &lightColor_);
    glBindTexture(GL_TEXTURE_2D, lightColor_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &lightFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, lightFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lightColor_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, gbufferDepth_, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);

    if (fullscreenVao_ == 0) {
        glGenVertexArrays(1, &fullscreenVao_);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

std::vector<ResourceMemoryRecord> DeferredRenderPath::profilingResources() const {
    if (deferredWidth_ <= 0 || deferredHeight_ <= 0 || gbufferFbo_ == 0) {
        return {};
    }

    std::vector<ResourceMemoryRecord> resources{
        ResourceMemoryRecord{"GBuffer Albedo", "Render Target", 0u, estimateTextureStorageBytes(deferredWidth_, deferredHeight_, 1, TextureStorageFormat::RGBA8)},
        ResourceMemoryRecord{"GBuffer Normal", "Render Target", 0u, estimateTextureStorageBytes(deferredWidth_, deferredHeight_, 1, TextureStorageFormat::RGBA16F)},
        ResourceMemoryRecord{"GBuffer Emissive AO", "Render Target", 0u, estimateTextureStorageBytes(deferredWidth_, deferredHeight_, 1, TextureStorageFormat::RGBA16F)},
        ResourceMemoryRecord{"GBuffer Clearcoat", "Render Target", 0u, estimateTextureStorageBytes(deferredWidth_, deferredHeight_, 1, TextureStorageFormat::RGBA16F)},
        ResourceMemoryRecord{"GBuffer Depth Color", "Render Target", 0u, estimateTextureStorageBytes(deferredWidth_, deferredHeight_, 1, TextureStorageFormat::R32F)},
        ResourceMemoryRecord{"GBuffer Depth", "Render Target", 0u, estimateTextureStorageBytes(deferredWidth_, deferredHeight_, 1, TextureStorageFormat::Depth24Stencil8)},
        ResourceMemoryRecord{"Light Accumulation", "Render Target", 0u, estimateTextureStorageBytes(deferredWidth_, deferredHeight_, 1, TextureStorageFormat::RGBA16F)},
    };
    if (lightBuffers_.buffer != 0 && lightBuffers_.size > 0) {
        resources.push_back(ResourceMemoryRecord{
            "Deferred Light Buffer",
            "Light Buffer",
            0u,
            static_cast<std::uint64_t>(lightBuffers_.size)
        });
    }
    return resources;
}

}  // namespace render
