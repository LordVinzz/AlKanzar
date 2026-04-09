#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/mat3x3.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace render {

enum class Format {
    Unknown = 0,
    R8,
    RGB8,
    RGBA8,
    RGBA16F,
};

enum class Filter {
    None = 0,
    Nearest,
    Linear,
};

enum class WrapMode {
    Repeat = 0,
    Clamp,
    Mirror,
};

enum class AlphaMode {
    Opaque = 0,
    Mask,
    Blend,
};

enum class TextureSemantic {
    BaseColor = 0,
    Normal,
    ORM,
    AO,
    Emissive,
    Alpha,
    Clearcoat,
    Height,
    Generic,
};

enum class TextureOrigin {
    Default = 0,
    Project,
    Generated,
    InlinePrivate,
};

enum class TextureBindingMode {
    Default = 0,
    ProjectTexture,
    InlineValue,
};

enum class MaterialTextureSlot {
    BaseColor = 0,
    MetallicRoughness,
    Normal,
    Ao,
    Emissive,
    Alpha,
    Clearcoat,
    DetailNormal,
    Height,
};

struct Texture {
    std::string name;
    int width{0};
    int height{0};
    int mipLevels{1};
    Format format{Format::RGBA8};
    void* data{nullptr};
    std::vector<std::uint8_t> bytes;
    std::uint32_t gpuHandle{0};
    bool srgb{false};
    bool generated{false};
    TextureSemantic semantic{TextureSemantic::Generic};
    TextureOrigin origin{TextureOrigin::Generated};

    [[nodiscard]] bool valid() const {
        return width > 0 && height > 0 && !bytes.empty();
    }
};

struct Sampler {
    Filter minFilter{Filter::Linear};
    Filter magFilter{Filter::Linear};
    Filter mipFilter{Filter::Linear};
    WrapMode wrapU{WrapMode::Repeat};
    WrapMode wrapV{WrapMode::Repeat};
    WrapMode wrapW{WrapMode::Repeat};
    float anisotropy{8.0f};
    std::uint32_t gpuHandle{0};
};

struct UVTransform {
    glm::vec2 offset{0.0f, 0.0f};
    glm::vec2 scale{1.0f, 1.0f};
    float rotation{0.0f};
};

struct TextureRef {
    std::shared_ptr<Texture> texture;
    std::shared_ptr<Sampler> sampler;
    int uvSet{0};
    UVTransform transform{};
    TextureBindingMode bindingMode{TextureBindingMode::Default};
    glm::vec4 inlineValue{0.0f};

    [[nodiscard]] bool valid() const {
        return bindingMode != TextureBindingMode::Default && texture != nullptr && texture->valid();
    }
};

struct MetallicRoughnessTexture {
    TextureRef texture;
};

struct Clearcoat {
    float factor{0.0f};
    float roughness{0.0f};
    TextureRef texture;
};

struct DetailNormal {
    TextureRef texture;
    float scale{1.0f};
};

struct Height {
    TextureRef texture;
    float scale{0.05f};
};

struct Material {
    std::string name;
    glm::vec3 baseColorFactor{1.0f};
    TextureRef baseColor;

    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    MetallicRoughnessTexture metallicRoughness;

    TextureRef normal;
    float normalScale{1.0f};

    TextureRef ao;
    float aoStrength{1.0f};

    glm::vec3 emissiveFactor{0.0f};
    TextureRef emissive;
    float emissiveStrength{1.0f};

    float alphaFactor{1.0f};
    TextureRef alpha;

    AlphaMode alphaMode{AlphaMode::Opaque};
    float alphaCutoff{0.5f};

    bool doubleSided{false};

    Clearcoat clearcoat{};
    DetailNormal detailNormal{};
    Height height{};
};

struct ShaderInputs {
    glm::vec3 baseColorFactor{1.0f};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};

    TextureRef baseColorTexture{};
    TextureRef normalTexture{};
    TextureRef metallicRoughnessTexture{};
    TextureRef aoTexture{};
    TextureRef emissiveTexture{};
    TextureRef alphaTexture{};
    TextureRef clearcoatTexture{};
    TextureRef detailNormalTexture{};
    TextureRef heightTexture{};

    float normalScale{1.0f};
    float aoStrength{1.0f};
    glm::vec3 emissiveFactor{0.0f};
    float emissiveStrength{1.0f};
    float alphaFactor{1.0f};
    AlphaMode alphaMode{AlphaMode::Opaque};
    float alphaCutoff{0.5f};
    bool doubleSided{false};
    float clearcoatFactor{0.0f};
    float clearcoatRoughness{0.0f};
    float detailNormalScale{1.0f};
    float heightScale{0.05f};
};

std::shared_ptr<Texture> loadTextureFromFile(
    const std::string& path,
    const std::string& name,
    bool srgb = false,
    TextureSemantic semantic = TextureSemantic::Generic
);
std::shared_ptr<Texture> makeSolidTexture(
    const std::string& name,
    const glm::vec3& color,
    bool srgb = false,
    TextureSemantic semantic = TextureSemantic::Generic,
    TextureOrigin origin = TextureOrigin::Generated
);
std::shared_ptr<Texture> makeSolidTexture(
    const std::string& name,
    const glm::vec4& color,
    bool srgb = false,
    TextureSemantic semantic = TextureSemantic::Generic,
    TextureOrigin origin = TextureOrigin::Generated
);
std::shared_ptr<Texture> generateNormalTexture(const Texture& source, const std::string& name, float strength = 4.0f);
std::shared_ptr<Texture> generateOcclusionTexture(const Texture& source, const std::string& name);
std::shared_ptr<Texture> generateMetallicRoughnessTexture(
    const Texture& source,
    const std::string& name,
    float metallicValue = 0.0f,
    float roughnessBias = 0.0f
);
std::shared_ptr<Texture> generateHeightTexture(const Texture& source, const std::string& name);

TextureRef makeTextureRef(
    const std::shared_ptr<Texture>& texture,
    const std::shared_ptr<Sampler>& sampler,
    int uvSet = 0,
    const UVTransform& transform = {}
);

glm::mat3 uvTransformMatrix(const UVTransform& transform);

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
);

const char* formatName(Format format);
const char* filterName(Filter filter);
const char* wrapModeName(WrapMode wrapMode);
const char* alphaModeName(AlphaMode mode);
const char* textureSemanticName(TextureSemantic semantic);
const char* textureOriginName(TextureOrigin origin);
const char* textureBindingModeName(TextureBindingMode mode);
const char* materialTextureSlotName(MaterialTextureSlot slot);

TextureSemantic textureSemanticForSlot(MaterialTextureSlot slot);
glm::vec4 defaultInlineValueForSlot(MaterialTextureSlot slot);
TextureRef* textureRefForSlot(Material& material, MaterialTextureSlot slot);
const TextureRef* textureRefForSlot(const Material& material, MaterialTextureSlot slot);

}  // namespace render
