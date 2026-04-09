#include "Material.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec4.hpp>
#include <spdlog/spdlog.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace render {

namespace {

glm::vec4 byteToVec4(const std::vector<std::uint8_t>& bytes, int width, int x, int y) {
    const int clampedX = std::clamp(x, 0, width - 1);
    const int height = static_cast<int>(bytes.size() / (static_cast<std::size_t>(width) * 4));
    const int clampedY = std::clamp(y, 0, height - 1);
    const int index = (clampedY * width + clampedX) * 4;
    return glm::vec4(
        static_cast<float>(bytes[index + 0]) / 255.0f,
        static_cast<float>(bytes[index + 1]) / 255.0f,
        static_cast<float>(bytes[index + 2]) / 255.0f,
        static_cast<float>(bytes[index + 3]) / 255.0f
    );
}

float luminance(const glm::vec4& color) {
    return glm::dot(glm::vec3(color), glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

std::shared_ptr<Texture> makeTexture(
    const std::string& name,
    int width,
    int height,
    bool srgb,
    std::vector<std::uint8_t>&& bytes,
    TextureSemantic semantic,
    TextureOrigin origin
) {
    auto texture = std::make_shared<Texture>();
    texture->name = name;
    texture->width = width;
    texture->height = height;
    texture->mipLevels = 1;
    texture->format = Format::RGBA8;
    texture->bytes = std::move(bytes);
    texture->data = texture->bytes.empty() ? nullptr : texture->bytes.data();
    texture->srgb = srgb;
    texture->semantic = semantic;
    texture->origin = origin;
    return texture;
}

}  // namespace

std::shared_ptr<Texture> loadTextureFromFile(
    const std::string& path,
    const std::string& name,
    bool srgb,
    TextureSemantic semantic
) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        spdlog::error("Material: failed to load texture '{}': {}", path, stbi_failure_reason());
        return nullptr;
    }

    std::vector<std::uint8_t> bytes(
        pixels,
        pixels + static_cast<std::ptrdiff_t>(width) * static_cast<std::ptrdiff_t>(height) * 4
    );
    stbi_image_free(pixels);

    auto texture = makeTexture(name, width, height, srgb, std::move(bytes), semantic, TextureOrigin::Project);
    texture->generated = false;
    return texture;
}

std::shared_ptr<Texture> makeSolidTexture(
    const std::string& name,
    const glm::vec3& color,
    bool srgb,
    TextureSemantic semantic,
    TextureOrigin origin
) {
    return makeSolidTexture(name, glm::vec4(color, 1.0f), srgb, semantic, origin);
}

std::shared_ptr<Texture> makeSolidTexture(
    const std::string& name,
    const glm::vec4& color,
    bool srgb,
    TextureSemantic semantic,
    TextureOrigin origin
) {
    std::vector<std::uint8_t> bytes(4);
    bytes[0] = static_cast<std::uint8_t>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
    bytes[1] = static_cast<std::uint8_t>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
    bytes[2] = static_cast<std::uint8_t>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
    bytes[3] = static_cast<std::uint8_t>(glm::clamp(color.a, 0.0f, 1.0f) * 255.0f);

    auto texture = makeTexture(name, 1, 1, srgb, std::move(bytes), semantic, origin);
    texture->generated = true;
    return texture;
}

std::shared_ptr<Texture> generateNormalTexture(const Texture& source, const std::string& name, float strength) {
    if (!source.valid() || source.bytes.size() < 4) {
        return nullptr;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(source.width) * static_cast<std::size_t>(source.height) * 4);
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const float left = luminance(byteToVec4(source.bytes, source.width, x - 1, y));
            const float right = luminance(byteToVec4(source.bytes, source.width, x + 1, y));
            const float up = luminance(byteToVec4(source.bytes, source.width, x, y - 1));
            const float down = luminance(byteToVec4(source.bytes, source.width, x, y + 1));

            const glm::vec3 normal = glm::normalize(glm::vec3((left - right) * strength, (up - down) * strength, 1.0f));
            const int index = (y * source.width + x) * 4;
            bytes[index + 0] = static_cast<std::uint8_t>((normal.x * 0.5f + 0.5f) * 255.0f);
            bytes[index + 1] = static_cast<std::uint8_t>((normal.y * 0.5f + 0.5f) * 255.0f);
            bytes[index + 2] = static_cast<std::uint8_t>((normal.z * 0.5f + 0.5f) * 255.0f);
            bytes[index + 3] = 255;
        }
    }

    auto texture = makeTexture(
        name,
        source.width,
        source.height,
        false,
        std::move(bytes),
        TextureSemantic::Normal,
        TextureOrigin::Generated
    );
    texture->generated = true;
    return texture;
}

std::shared_ptr<Texture> generateOcclusionTexture(const Texture& source, const std::string& name) {
    if (!source.valid() || source.bytes.size() < 4) {
        return nullptr;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(source.width) * static_cast<std::size_t>(source.height) * 4);
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            float neighborhood = 0.0f;
            int taps = 0;
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    neighborhood += luminance(byteToVec4(source.bytes, source.width, x + ox, y + oy));
                    taps++;
                }
            }
            const float avg = neighborhood / static_cast<float>(std::max(taps, 1));
            const float center = luminance(byteToVec4(source.bytes, source.width, x, y));
            const float ao = glm::clamp(0.55f + avg * 0.45f - (center - avg) * 0.25f, 0.15f, 1.0f);

            const std::uint8_t value = static_cast<std::uint8_t>(ao * 255.0f);
            const int index = (y * source.width + x) * 4;
            bytes[index + 0] = value;
            bytes[index + 1] = value;
            bytes[index + 2] = value;
            bytes[index + 3] = 255;
        }
    }

    auto texture = makeTexture(
        name,
        source.width,
        source.height,
        false,
        std::move(bytes),
        TextureSemantic::AO,
        TextureOrigin::Generated
    );
    texture->generated = true;
    return texture;
}

std::shared_ptr<Texture> generateMetallicRoughnessTexture(
    const Texture& source,
    const std::string& name,
    float metallicValue,
    float roughnessBias
) {
    if (!source.valid() || source.bytes.size() < 4) {
        return nullptr;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(source.width) * static_cast<std::size_t>(source.height) * 4);
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const float center = luminance(byteToVec4(source.bytes, source.width, x, y));
            const float roughness = glm::clamp(0.35f + (1.0f - center) * 0.45f + roughnessBias, 0.08f, 1.0f);
            const float metallic = glm::clamp(metallicValue, 0.0f, 1.0f);
            const float ao = glm::clamp(0.65f + center * 0.35f, 0.2f, 1.0f);

            const int index = (y * source.width + x) * 4;
            bytes[index + 0] = static_cast<std::uint8_t>(ao * 255.0f);
            bytes[index + 1] = static_cast<std::uint8_t>(roughness * 255.0f);
            bytes[index + 2] = static_cast<std::uint8_t>(metallic * 255.0f);
            bytes[index + 3] = 255;
        }
    }

    auto texture = makeTexture(
        name,
        source.width,
        source.height,
        false,
        std::move(bytes),
        TextureSemantic::ORM,
        TextureOrigin::Generated
    );
    texture->generated = true;
    return texture;
}

std::shared_ptr<Texture> generateHeightTexture(const Texture& source, const std::string& name) {
    if (!source.valid() || source.bytes.size() < 4) {
        return nullptr;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(source.width) * static_cast<std::size_t>(source.height) * 4);
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const float height = luminance(byteToVec4(source.bytes, source.width, x, y));
            const std::uint8_t value = static_cast<std::uint8_t>(glm::clamp(height, 0.0f, 1.0f) * 255.0f);
            const int index = (y * source.width + x) * 4;
            bytes[index + 0] = value;
            bytes[index + 1] = value;
            bytes[index + 2] = value;
            bytes[index + 3] = 255;
        }
    }

    auto texture = makeTexture(
        name,
        source.width,
        source.height,
        false,
        std::move(bytes),
        TextureSemantic::Height,
        TextureOrigin::Generated
    );
    texture->generated = true;
    return texture;
}

TextureRef makeTextureRef(
    const std::shared_ptr<Texture>& texture,
    const std::shared_ptr<Sampler>& sampler,
    int uvSet,
    const UVTransform& transform
) {
    TextureRef ref{};
    ref.texture = texture;
    ref.sampler = sampler;
    ref.uvSet = uvSet;
    ref.transform = transform;
    ref.bindingMode = texture ? TextureBindingMode::ProjectTexture : TextureBindingMode::Default;
    return ref;
}

glm::mat3 uvTransformMatrix(const UVTransform& transform) {
    const float c = std::cos(transform.rotation);
    const float s = std::sin(transform.rotation);
    return glm::mat3(
        transform.scale.x * c, transform.scale.x * s, 0.0f,
        -transform.scale.y * s, transform.scale.y * c, 0.0f,
        transform.offset.x, transform.offset.y, 1.0f
    );
}

ShaderInputs resolveShaderInputs(
    const Material& material,
    const TextureRef& defaultBaseColor,
    const TextureRef& defaultNormal,
    const TextureRef& defaultMetallicRoughness,
    const TextureRef& defaultAo,
    const TextureRef& defaultEmissive,
    const TextureRef& defaultAlpha,
    const TextureRef& defaultClearcoat,
    const TextureRef& defaultDetailNormal,
    const TextureRef& defaultHeight
) {
    const auto resolveTexture = [](const TextureRef& ref, const TextureRef& fallback) {
        if (ref.valid()) {
            return ref;
        }

        TextureRef resolved = fallback;
        resolved.uvSet = ref.uvSet;
        resolved.transform = ref.transform;
        if (ref.sampler) {
            resolved.sampler = ref.sampler;
        }
        resolved.bindingMode = TextureBindingMode::ProjectTexture;
        return resolved;
    };

    ShaderInputs inputs{};
    inputs.baseColorFactor = material.baseColorFactor;
    inputs.metallicFactor = material.metallicFactor;
    inputs.roughnessFactor = material.roughnessFactor;
    inputs.baseColorTexture = resolveTexture(material.baseColor, defaultBaseColor);
    inputs.normalTexture = resolveTexture(material.normal, defaultNormal);
    inputs.metallicRoughnessTexture = resolveTexture(material.metallicRoughness.texture, defaultMetallicRoughness);
    inputs.aoTexture = resolveTexture(material.ao, defaultAo);
    inputs.emissiveTexture = resolveTexture(material.emissive, defaultEmissive);
    inputs.alphaTexture = resolveTexture(material.alpha, defaultAlpha);
    inputs.clearcoatTexture = resolveTexture(material.clearcoat.texture, defaultClearcoat);
    inputs.detailNormalTexture = resolveTexture(material.detailNormal.texture, defaultDetailNormal);
    inputs.heightTexture = resolveTexture(material.height.texture, defaultHeight);
    inputs.normalScale = material.normalScale;
    inputs.aoStrength = material.aoStrength;
    inputs.emissiveFactor = material.emissiveFactor;
    inputs.emissiveStrength = material.emissiveStrength;
    inputs.alphaFactor = material.alphaFactor;
    inputs.alphaMode = material.alphaMode;
    inputs.alphaCutoff = material.alphaCutoff;
    inputs.doubleSided = material.doubleSided;
    inputs.clearcoatFactor = material.clearcoat.factor;
    inputs.clearcoatRoughness = material.clearcoat.roughness;
    inputs.detailNormalScale = material.detailNormal.scale;
    inputs.heightScale = material.height.scale;
    return inputs;
}

const char* formatName(Format format) {
    switch (format) {
        case Format::R8:
            return "R8";
        case Format::RGB8:
            return "RGB8";
        case Format::RGBA8:
            return "RGBA8";
        case Format::RGBA16F:
            return "RGBA16F";
        default:
            return "Unknown";
    }
}

const char* filterName(Filter filter) {
    switch (filter) {
        case Filter::Nearest:
            return "Nearest";
        case Filter::Linear:
            return "Linear";
        default:
            return "None";
    }
}

const char* wrapModeName(WrapMode wrapMode) {
    switch (wrapMode) {
        case WrapMode::Clamp:
            return "Clamp";
        case WrapMode::Mirror:
            return "Mirror";
        default:
            return "Repeat";
    }
}

const char* alphaModeName(AlphaMode mode) {
    switch (mode) {
        case AlphaMode::Mask:
            return "Mask";
        case AlphaMode::Blend:
            return "Blend";
        default:
            return "Opaque";
    }
}

const char* textureSemanticName(TextureSemantic semantic) {
    switch (semantic) {
        case TextureSemantic::BaseColor:
            return "BaseColor";
        case TextureSemantic::Normal:
            return "Normal";
        case TextureSemantic::ORM:
            return "ORM";
        case TextureSemantic::AO:
            return "AO";
        case TextureSemantic::Emissive:
            return "Emissive";
        case TextureSemantic::Alpha:
            return "Alpha";
        case TextureSemantic::Clearcoat:
            return "Clearcoat";
        case TextureSemantic::Height:
            return "Height";
        default:
            return "Generic";
    }
}

const char* textureOriginName(TextureOrigin origin) {
    switch (origin) {
        case TextureOrigin::Default:
            return "Default";
        case TextureOrigin::Project:
            return "Project";
        case TextureOrigin::Generated:
            return "Generated";
        case TextureOrigin::InlinePrivate:
            return "Inline";
        default:
            return "Unknown";
    }
}

const char* textureBindingModeName(TextureBindingMode mode) {
    switch (mode) {
        case TextureBindingMode::ProjectTexture:
            return "ProjectTexture";
        case TextureBindingMode::InlineValue:
            return "InlineValue";
        default:
            return "Default";
    }
}

const char* materialTextureSlotName(MaterialTextureSlot slot) {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
            return "Base Color";
        case MaterialTextureSlot::MetallicRoughness:
            return "Metallic / Roughness";
        case MaterialTextureSlot::Normal:
            return "Normal";
        case MaterialTextureSlot::Ao:
            return "Ambient Occlusion";
        case MaterialTextureSlot::Emissive:
            return "Emissive";
        case MaterialTextureSlot::Alpha:
            return "Alpha";
        case MaterialTextureSlot::Clearcoat:
            return "Clearcoat";
        case MaterialTextureSlot::DetailNormal:
            return "Detail Normal";
        case MaterialTextureSlot::Height:
            return "Height";
        default:
            return "Unknown";
    }
}

TextureSemantic textureSemanticForSlot(MaterialTextureSlot slot) {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
            return TextureSemantic::BaseColor;
        case MaterialTextureSlot::Normal:
        case MaterialTextureSlot::DetailNormal:
            return TextureSemantic::Normal;
        case MaterialTextureSlot::MetallicRoughness:
            return TextureSemantic::ORM;
        case MaterialTextureSlot::Ao:
            return TextureSemantic::AO;
        case MaterialTextureSlot::Emissive:
            return TextureSemantic::Emissive;
        case MaterialTextureSlot::Alpha:
            return TextureSemantic::Alpha;
        case MaterialTextureSlot::Clearcoat:
            return TextureSemantic::Clearcoat;
        case MaterialTextureSlot::Height:
            return TextureSemantic::Height;
        default:
            return TextureSemantic::Generic;
    }
}

glm::vec4 defaultInlineValueForSlot(MaterialTextureSlot slot) {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
            return glm::vec4(1.0f);
        case MaterialTextureSlot::MetallicRoughness:
            return glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
        case MaterialTextureSlot::Normal:
        case MaterialTextureSlot::DetailNormal:
            return glm::vec4(0.5f, 0.5f, 1.0f, 1.0f);
        case MaterialTextureSlot::Ao:
        case MaterialTextureSlot::Alpha:
            return glm::vec4(1.0f);
        case MaterialTextureSlot::Emissive:
            return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        case MaterialTextureSlot::Clearcoat:
            return glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
        case MaterialTextureSlot::Height:
            return glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
        default:
            return glm::vec4(0.0f);
    }
}

TextureRef* textureRefForSlot(Material& material, MaterialTextureSlot slot) {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
            return &material.baseColor;
        case MaterialTextureSlot::MetallicRoughness:
            return &material.metallicRoughness.texture;
        case MaterialTextureSlot::Normal:
            return &material.normal;
        case MaterialTextureSlot::Ao:
            return &material.ao;
        case MaterialTextureSlot::Emissive:
            return &material.emissive;
        case MaterialTextureSlot::Alpha:
            return &material.alpha;
        case MaterialTextureSlot::Clearcoat:
            return &material.clearcoat.texture;
        case MaterialTextureSlot::DetailNormal:
            return &material.detailNormal.texture;
        case MaterialTextureSlot::Height:
            return &material.height.texture;
        default:
            return nullptr;
    }
}

const TextureRef* textureRefForSlot(const Material& material, MaterialTextureSlot slot) {
    return textureRefForSlot(const_cast<Material&>(material), slot);
}

}  // namespace render
