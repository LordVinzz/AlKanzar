#include "Material.hpp"

#include <cmath>

#include <glm/mat3x3.hpp>
#include <glm/trigonometric.hpp>

namespace render {

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
    ref.bindingMode = texture
        ? TextureBindingMode::ProjectTexture
        : TextureBindingMode::Default;
    return ref;
}

glm::mat3 uvTransformMatrix(const UVTransform& transform) {
    const float cosine = std::cos(transform.rotation);
    const float sine = std::sin(transform.rotation);
    return glm::mat3(
        transform.scale.x * cosine, transform.scale.x * sine, 0.0f,
        -transform.scale.y * sine, transform.scale.y * cosine, 0.0f,
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
        if (ref.valid()) return ref;
        TextureRef resolved = fallback;
        resolved.uvSet = ref.uvSet;
        resolved.transform = ref.transform;
        if (ref.sampler) resolved.sampler = ref.sampler;
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
        case Format::R8: return "R8";
        case Format::RGB8: return "RGB8";
        case Format::RGBA8: return "RGBA8";
        case Format::RGBA16F: return "RGBA16F";
        default: return "Unknown";
    }
}

const char* filterName(Filter filter) {
    switch (filter) {
        case Filter::Nearest: return "Nearest";
        case Filter::Linear: return "Linear";
        default: return "None";
    }
}

const char* wrapModeName(WrapMode wrapMode) {
    switch (wrapMode) {
        case WrapMode::Clamp: return "Clamp";
        case WrapMode::Mirror: return "Mirror";
        default: return "Repeat";
    }
}

const char* alphaModeName(AlphaMode mode) {
    switch (mode) {
        case AlphaMode::Mask: return "Mask";
        case AlphaMode::Blend: return "Blend";
        default: return "Opaque";
    }
}

const char* textureSemanticName(TextureSemantic semantic) {
    switch (semantic) {
        case TextureSemantic::BaseColor: return "BaseColor";
        case TextureSemantic::Normal: return "Normal";
        case TextureSemantic::ORM: return "ORM";
        case TextureSemantic::AO: return "AO";
        case TextureSemantic::Emissive: return "Emissive";
        case TextureSemantic::Alpha: return "Alpha";
        case TextureSemantic::Clearcoat: return "Clearcoat";
        case TextureSemantic::Height: return "Height";
        default: return "Generic";
    }
}

const char* textureOriginName(TextureOrigin origin) {
    switch (origin) {
        case TextureOrigin::Default: return "Default";
        case TextureOrigin::Project: return "Project";
        case TextureOrigin::Generated: return "Generated";
        case TextureOrigin::InlinePrivate: return "Inline";
        default: return "Unknown";
    }
}

const char* textureBindingModeName(TextureBindingMode mode) {
    switch (mode) {
        case TextureBindingMode::ProjectTexture: return "ProjectTexture";
        case TextureBindingMode::InlineValue: return "InlineValue";
        default: return "Default";
    }
}

const char* materialTextureSlotName(MaterialTextureSlot slot) {
    switch (slot) {
        case MaterialTextureSlot::BaseColor: return "Base Color";
        case MaterialTextureSlot::MetallicRoughness: return "Metallic / Roughness";
        case MaterialTextureSlot::Normal: return "Normal";
        case MaterialTextureSlot::Ao: return "Ambient Occlusion";
        case MaterialTextureSlot::Emissive: return "Emissive";
        case MaterialTextureSlot::Alpha: return "Alpha";
        case MaterialTextureSlot::Clearcoat: return "Clearcoat";
        case MaterialTextureSlot::DetailNormal: return "Detail Normal";
        case MaterialTextureSlot::Height: return "Height";
        default: return "Unknown";
    }
}

TextureSemantic textureSemanticForSlot(MaterialTextureSlot slot) {
    switch (slot) {
        case MaterialTextureSlot::BaseColor: return TextureSemantic::BaseColor;
        case MaterialTextureSlot::Normal:
        case MaterialTextureSlot::DetailNormal: return TextureSemantic::Normal;
        case MaterialTextureSlot::MetallicRoughness: return TextureSemantic::ORM;
        case MaterialTextureSlot::Ao: return TextureSemantic::AO;
        case MaterialTextureSlot::Emissive: return TextureSemantic::Emissive;
        case MaterialTextureSlot::Alpha: return TextureSemantic::Alpha;
        case MaterialTextureSlot::Clearcoat: return TextureSemantic::Clearcoat;
        case MaterialTextureSlot::Height: return TextureSemantic::Height;
        default: return TextureSemantic::Generic;
    }
}

glm::vec4 defaultInlineValueForSlot(MaterialTextureSlot slot) {
    switch (slot) {
        case MaterialTextureSlot::BaseColor:
        case MaterialTextureSlot::Ao:
        case MaterialTextureSlot::Alpha: return glm::vec4(1.0f);
        case MaterialTextureSlot::MetallicRoughness:
        case MaterialTextureSlot::Clearcoat: return glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
        case MaterialTextureSlot::Normal:
        case MaterialTextureSlot::DetailNormal: return glm::vec4(0.5f, 0.5f, 1.0f, 1.0f);
        case MaterialTextureSlot::Emissive: return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        case MaterialTextureSlot::Height: return glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
        default: return glm::vec4(0.0f);
    }
}

TextureRef* textureRefForSlot(Material& material, MaterialTextureSlot slot) {
    switch (slot) {
        case MaterialTextureSlot::BaseColor: return &material.baseColor;
        case MaterialTextureSlot::MetallicRoughness: return &material.metallicRoughness.texture;
        case MaterialTextureSlot::Normal: return &material.normal;
        case MaterialTextureSlot::Ao: return &material.ao;
        case MaterialTextureSlot::Emissive: return &material.emissive;
        case MaterialTextureSlot::Alpha: return &material.alpha;
        case MaterialTextureSlot::Clearcoat: return &material.clearcoat.texture;
        case MaterialTextureSlot::DetailNormal: return &material.detailNormal.texture;
        case MaterialTextureSlot::Height: return &material.height.texture;
        default: return nullptr;
    }
}

const TextureRef* textureRefForSlot(const Material& material, MaterialTextureSlot slot) {
    return textureRefForSlot(const_cast<Material&>(material), slot);
}

}  // namespace render
