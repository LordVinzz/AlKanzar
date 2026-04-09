#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL_opengl.h>

#include <memory>
#include <vector>

#include "Material.hpp"

namespace render {

class RenderResourceRegistry {
public:
    ~RenderResourceRegistry();

    bool initializeDefaults();
    void destroy();

    std::shared_ptr<Texture> registerTexture(const std::shared_ptr<Texture>& texture);
    std::shared_ptr<Sampler> registerSampler(const std::shared_ptr<Sampler>& sampler);

    [[nodiscard]] const std::shared_ptr<Sampler>& defaultSampler() const { return defaultSampler_; }
    [[nodiscard]] const TextureRef& defaultTextureForSlot(MaterialTextureSlot slot) const;
    [[nodiscard]] const TextureRef& defaultTextureForUnit(int unit) const;
    [[nodiscard]] std::vector<std::shared_ptr<Texture>> textureCatalog(TextureSemantic preferredSemantic) const;
    [[nodiscard]] void* texturePreviewId(const std::shared_ptr<Texture>& texture);

    bool ensureTextureUploaded(Texture& texture) const;
    bool ensureSamplerUploaded(Sampler& sampler) const;
    void bindTextureRef(int unit, const TextureRef& ref) const;

private:
    std::vector<std::shared_ptr<Texture>> textures_{};
    std::vector<std::shared_ptr<Sampler>> samplers_{};
    std::shared_ptr<Sampler> defaultSampler_{};
    TextureRef defaultBaseColorTexture_{};
    TextureRef defaultNormalTexture_{};
    TextureRef defaultMetallicRoughnessTexture_{};
    TextureRef defaultAoTexture_{};
    TextureRef defaultEmissiveTexture_{};
    TextureRef defaultAlphaTexture_{};
    TextureRef defaultClearcoatTexture_{};
    TextureRef defaultDetailNormalTexture_{};
    TextureRef defaultHeightTexture_{};
};

}  // namespace render
