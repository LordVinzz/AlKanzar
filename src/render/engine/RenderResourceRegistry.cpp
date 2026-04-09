#include "RenderResourceRegistry.hpp"

#include <algorithm>
#include <cmath>

namespace render {

RenderResourceRegistry::~RenderResourceRegistry() {
    destroy();
}

bool RenderResourceRegistry::initializeDefaults() {
    destroy();

    auto sampler = std::make_shared<Sampler>();
    sampler->minFilter = Filter::Linear;
    sampler->magFilter = Filter::Linear;
    sampler->mipFilter = Filter::Linear;
    sampler->wrapU = WrapMode::Repeat;
    sampler->wrapV = WrapMode::Repeat;
    sampler->wrapW = WrapMode::Repeat;
    sampler->anisotropy = 8.0f;
    defaultSampler_ = registerSampler(sampler);

    defaultBaseColorTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultWhite",
        glm::vec4(1.0f),
        true,
        TextureSemantic::BaseColor,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultNormalTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultNormal",
        glm::vec4(0.5f, 0.5f, 1.0f, 1.0f),
        false,
        TextureSemantic::Normal,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultMetallicRoughnessTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultORM",
        glm::vec4(1.0f, 1.0f, 0.0f, 1.0f),
        false,
        TextureSemantic::ORM,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultAoTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultAO",
        glm::vec4(1.0f),
        false,
        TextureSemantic::AO,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultEmissiveTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultBlack",
        glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
        false,
        TextureSemantic::Emissive,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultAlphaTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultAlpha",
        glm::vec4(1.0f),
        false,
        TextureSemantic::Alpha,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultClearcoatTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultClearcoat",
        glm::vec4(1.0f, 1.0f, 0.0f, 1.0f),
        false,
        TextureSemantic::Clearcoat,
        TextureOrigin::Default
    )), defaultSampler_);
    defaultDetailNormalTexture_ = defaultNormalTexture_;
    defaultHeightTexture_ = makeTextureRef(registerTexture(makeSolidTexture(
        "DefaultHeight",
        glm::vec4(0.5f, 0.5f, 0.5f, 1.0f),
        false,
        TextureSemantic::Height,
        TextureOrigin::Default
    )), defaultSampler_);
    return true;
}

void RenderResourceRegistry::destroy() {
    for (const auto& texture : textures_) {
        if (texture && texture->gpuHandle != 0) {
            const GLuint handle = static_cast<GLuint>(texture->gpuHandle);
            glDeleteTextures(1, &handle);
            texture->gpuHandle = 0;
        }
    }

    for (const auto& sampler : samplers_) {
        if (sampler && sampler->gpuHandle != 0) {
            const GLuint handle = static_cast<GLuint>(sampler->gpuHandle);
            glDeleteSamplers(1, &handle);
            sampler->gpuHandle = 0;
        }
    }

    defaultSampler_.reset();
    defaultBaseColorTexture_ = {};
    defaultNormalTexture_ = {};
    defaultMetallicRoughnessTexture_ = {};
    defaultAoTexture_ = {};
    defaultEmissiveTexture_ = {};
    defaultAlphaTexture_ = {};
    defaultClearcoatTexture_ = {};
    defaultDetailNormalTexture_ = {};
    defaultHeightTexture_ = {};
    textures_.clear();
    samplers_.clear();
}

std::shared_ptr<Texture> RenderResourceRegistry::registerTexture(const std::shared_ptr<Texture>& texture) {
    if (texture) {
        textures_.push_back(texture);
    }
    return texture;
}

std::shared_ptr<Sampler> RenderResourceRegistry::registerSampler(const std::shared_ptr<Sampler>& sampler) {
    if (sampler) {
        samplers_.push_back(sampler);
    }
    return sampler;
}

bool RenderResourceRegistry::ensureTextureUploaded(Texture& texture) const {
    if (texture.gpuHandle != 0) {
        return true;
    }
    if (!texture.valid()) {
        return false;
    }

    GLenum internalFormat = GL_RGBA8;
    GLenum format = GL_RGBA;
    GLenum type = GL_UNSIGNED_BYTE;

    switch (texture.format) {
        case Format::R8:
            internalFormat = GL_R8;
            format = GL_RED;
            break;
        case Format::RGB8:
            internalFormat = texture.srgb ? GL_SRGB8 : GL_RGB8;
            format = GL_RGB;
            break;
        case Format::RGBA16F:
            internalFormat = GL_RGBA16F;
            format = GL_RGBA;
            type = GL_HALF_FLOAT;
            break;
        case Format::RGBA8:
        case Format::Unknown:
        default:
            internalFormat = texture.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
            format = GL_RGBA;
            break;
    }

    GLuint handle = 0;
    glGenTextures(1, &handle);
    glBindTexture(GL_TEXTURE_2D, handle);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        internalFormat,
        texture.width,
        texture.height,
        0,
        format,
        type,
        texture.bytes.data()
    );
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    const int mipLevels = 1 + static_cast<int>(std::floor(std::log2(static_cast<float>(std::max(texture.width, texture.height)))));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, std::max(mipLevels - 1, 0));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    texture.gpuHandle = handle;
    texture.mipLevels = mipLevels;
    return true;
}

bool RenderResourceRegistry::ensureSamplerUploaded(Sampler& sampler) const {
    if (sampler.gpuHandle != 0) {
        return true;
    }

    GLuint handle = 0;
    glGenSamplers(1, &handle);

    const auto toWrap = [](WrapMode mode) {
        switch (mode) {
            case WrapMode::Clamp:
                return static_cast<GLint>(GL_CLAMP_TO_EDGE);
            case WrapMode::Mirror:
                return static_cast<GLint>(GL_MIRRORED_REPEAT);
            case WrapMode::Repeat:
            default:
                return static_cast<GLint>(GL_REPEAT);
        }
    };

    const auto toMagFilter = [](Filter filter) {
        return filter == Filter::Nearest ? static_cast<GLint>(GL_NEAREST) : static_cast<GLint>(GL_LINEAR);
    };

    const auto toMinFilter = [](Filter minFilter, Filter mipFilter) {
        if (mipFilter == Filter::None) {
            return minFilter == Filter::Nearest ? static_cast<GLint>(GL_NEAREST) : static_cast<GLint>(GL_LINEAR);
        }
        if (minFilter == Filter::Nearest && mipFilter == Filter::Nearest) {
            return static_cast<GLint>(GL_NEAREST_MIPMAP_NEAREST);
        }
        if (minFilter == Filter::Nearest && mipFilter == Filter::Linear) {
            return static_cast<GLint>(GL_NEAREST_MIPMAP_LINEAR);
        }
        if (minFilter == Filter::Linear && mipFilter == Filter::Nearest) {
            return static_cast<GLint>(GL_LINEAR_MIPMAP_NEAREST);
        }
        return static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR);
    };

    glSamplerParameteri(handle, GL_TEXTURE_WRAP_S, toWrap(sampler.wrapU));
    glSamplerParameteri(handle, GL_TEXTURE_WRAP_T, toWrap(sampler.wrapV));
    glSamplerParameteri(handle, GL_TEXTURE_WRAP_R, toWrap(sampler.wrapW));
    glSamplerParameteri(handle, GL_TEXTURE_MIN_FILTER, toMinFilter(sampler.minFilter, sampler.mipFilter));
    glSamplerParameteri(handle, GL_TEXTURE_MAG_FILTER, toMagFilter(sampler.magFilter));

#ifdef GL_TEXTURE_MAX_ANISOTROPY_EXT
    GLfloat maxAnisotropy = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropy);
    glSamplerParameterf(handle, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(maxAnisotropy, sampler.anisotropy));
#endif

    sampler.gpuHandle = handle;
    return true;
}

const TextureRef& RenderResourceRegistry::defaultTextureForSlot(MaterialTextureSlot slot) const {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
            return defaultBaseColorTexture_;
        case MaterialTextureSlot::MetallicRoughness:
            return defaultMetallicRoughnessTexture_;
        case MaterialTextureSlot::Normal:
            return defaultNormalTexture_;
        case MaterialTextureSlot::Ao:
            return defaultAoTexture_;
        case MaterialTextureSlot::Emissive:
            return defaultEmissiveTexture_;
        case MaterialTextureSlot::Alpha:
            return defaultAlphaTexture_;
        case MaterialTextureSlot::Clearcoat:
            return defaultClearcoatTexture_;
        case MaterialTextureSlot::DetailNormal:
            return defaultDetailNormalTexture_;
        case MaterialTextureSlot::Height:
        default:
            return defaultHeightTexture_;
    }
}

const TextureRef& RenderResourceRegistry::defaultTextureForUnit(int unit) const {
    switch (unit) {
        case 0:
            return defaultBaseColorTexture_;
        case 1:
            return defaultMetallicRoughnessTexture_;
        case 2:
            return defaultNormalTexture_;
        case 3:
            return defaultAoTexture_;
        case 4:
            return defaultEmissiveTexture_;
        case 5:
            return defaultAlphaTexture_;
        case 6:
            return defaultClearcoatTexture_;
        case 7:
            return defaultDetailNormalTexture_;
        case 8:
        default:
            return defaultHeightTexture_;
    }
}

void RenderResourceRegistry::bindTextureRef(int unit, const TextureRef& ref) const {
    const TextureRef* selected = &ref;
    if (!selected->valid() || !ensureTextureUploaded(*selected->texture)) {
        selected = &defaultTextureForUnit(unit);
    }

    if (!selected->texture || !selected->texture->valid() || !ensureTextureUploaded(*selected->texture)) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindSampler(unit, 0);
        return;
    }

    if (selected->sampler) {
        ensureSamplerUploaded(*selected->sampler);
    }

    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(selected->texture->gpuHandle));
    glBindSampler(unit, selected->sampler ? static_cast<GLuint>(selected->sampler->gpuHandle) : 0);
}

std::vector<std::shared_ptr<Texture>> RenderResourceRegistry::textureCatalog(TextureSemantic preferredSemantic) const {
    std::vector<std::shared_ptr<Texture>> catalog;
    catalog.reserve(textures_.size());
    for (const auto& texture : textures_) {
        if (!texture || !texture->valid() || texture->origin == TextureOrigin::InlinePrivate) {
            continue;
        }
        catalog.push_back(texture);
    }

    std::stable_sort(catalog.begin(), catalog.end(), [preferredSemantic](const auto& lhs, const auto& rhs) {
        const bool lhsPreferred = lhs->semantic == preferredSemantic;
        const bool rhsPreferred = rhs->semantic == preferredSemantic;
        if (lhsPreferred != rhsPreferred) {
            return lhsPreferred && !rhsPreferred;
        }
        if (lhs->origin != rhs->origin) {
            return static_cast<int>(lhs->origin) < static_cast<int>(rhs->origin);
        }
        if (lhs->name != rhs->name) {
            return lhs->name < rhs->name;
        }
        if (lhs->width != rhs->width) {
            return lhs->width < rhs->width;
        }
        return lhs->height < rhs->height;
    });

    return catalog;
}

void* RenderResourceRegistry::texturePreviewId(const std::shared_ptr<Texture>& texture) {
    if (!texture || !ensureTextureUploaded(*texture)) {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<intptr_t>(texture->gpuHandle));
}

}  // namespace render
