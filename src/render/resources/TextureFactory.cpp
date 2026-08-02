#include "Material.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec4.hpp>
#include <spdlog/spdlog.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace render {
namespace {

glm::vec4 sampleRgba(
    const std::vector<std::uint8_t>& bytes,
    int width,
    int x,
    int y
) {
    const int clampedX = std::clamp(x, 0, width - 1);
    const int height = static_cast<int>(
        bytes.size() / (static_cast<std::size_t>(width) * 4u));
    const int clampedY = std::clamp(y, 0, height - 1);
    const int index = (clampedY * width + clampedX) * 4;
    return glm::vec4(
        static_cast<float>(bytes[index]) / 255.0f,
        static_cast<float>(bytes[index + 1]) / 255.0f,
        static_cast<float>(bytes[index + 2]) / 255.0f,
        static_cast<float>(bytes[index + 3]) / 255.0f);
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

std::optional<std::vector<std::uint8_t>> makeGeneratedBuffer(
    const Texture& source
) {
    if (!source.valid() || source.bytes.size() < 4u) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(
        static_cast<std::size_t>(source.width) *
        static_cast<std::size_t>(source.height) * 4u);
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
    if (pixels == nullptr) {
        spdlog::error("Material: failed to load texture '{}': {}", path, stbi_failure_reason());
        return nullptr;
    }
    std::vector<std::uint8_t> bytes(
        pixels,
        pixels + static_cast<std::ptrdiff_t>(width) *
            static_cast<std::ptrdiff_t>(height) * 4);
    stbi_image_free(pixels);
    auto texture = makeTexture(
        name, width, height, srgb, std::move(bytes), semantic,
        TextureOrigin::Project);
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
    std::vector<std::uint8_t> bytes(4u);
    for (int component = 0; component < 4; ++component) {
        bytes[static_cast<std::size_t>(component)] = static_cast<std::uint8_t>(
            glm::clamp(color[component], 0.0f, 1.0f) * 255.0f);
    }
    auto texture = makeTexture(name, 1, 1, srgb, std::move(bytes), semantic, origin);
    texture->generated = true;
    return texture;
}

std::shared_ptr<Texture> generateNormalTexture(
    const Texture& source,
    const std::string& name,
    float strength
) {
    auto bytes = makeGeneratedBuffer(source);
    if (!bytes.has_value()) return nullptr;
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const float left = luminance(sampleRgba(source.bytes, source.width, x - 1, y));
            const float right = luminance(sampleRgba(source.bytes, source.width, x + 1, y));
            const float up = luminance(sampleRgba(source.bytes, source.width, x, y - 1));
            const float down = luminance(sampleRgba(source.bytes, source.width, x, y + 1));
            const glm::vec3 normal = glm::normalize(glm::vec3(
                (left - right) * strength, (up - down) * strength, 1.0f));
            const int index = (y * source.width + x) * 4;
            (*bytes)[index] = static_cast<std::uint8_t>((normal.x * 0.5f + 0.5f) * 255.0f);
            (*bytes)[index + 1] = static_cast<std::uint8_t>((normal.y * 0.5f + 0.5f) * 255.0f);
            (*bytes)[index + 2] = static_cast<std::uint8_t>((normal.z * 0.5f + 0.5f) * 255.0f);
            (*bytes)[index + 3] = 255u;
        }
    }
    auto texture = makeTexture(name, source.width, source.height, false,
        std::move(*bytes), TextureSemantic::Normal, TextureOrigin::Generated);
    texture->generated = true;
    return texture;
}

std::shared_ptr<Texture> generateOcclusionTexture(
    const Texture& source,
    const std::string& name
) {
    auto bytes = makeGeneratedBuffer(source);
    if (!bytes.has_value()) return nullptr;
    for (int y = 0; y < source.height; ++y) for (int x = 0; x < source.width; ++x) {
        float neighborhood = 0.0f;
        for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox)
            neighborhood += luminance(sampleRgba(source.bytes, source.width, x + ox, y + oy));
        const float average = neighborhood / 9.0f;
        const float center = luminance(sampleRgba(source.bytes, source.width, x, y));
        const std::uint8_t value = static_cast<std::uint8_t>(glm::clamp(
            0.55f + average * 0.45f - (center - average) * 0.25f,
            0.15f, 1.0f) * 255.0f);
        const int index = (y * source.width + x) * 4;
        (*bytes)[index] = value; (*bytes)[index + 1] = value;
        (*bytes)[index + 2] = value; (*bytes)[index + 3] = 255u;
    }
    auto texture = makeTexture(name, source.width, source.height, false,
        std::move(*bytes), TextureSemantic::AO, TextureOrigin::Generated);
    texture->generated = true;
    return texture;
}

std::shared_ptr<Texture> generateMetallicRoughnessTexture(
    const Texture& source, const std::string& name, float metallicValue,
    float roughnessBias
) {
    auto bytes = makeGeneratedBuffer(source);
    if (!bytes.has_value()) return nullptr;
    for (int y = 0; y < source.height; ++y) for (int x = 0; x < source.width; ++x) {
        const float center = luminance(sampleRgba(source.bytes, source.width, x, y));
        const float roughness = glm::clamp(0.35f + (1.0f - center) * 0.45f + roughnessBias, 0.08f, 1.0f);
        const float metallic = glm::clamp(metallicValue, 0.0f, 1.0f);
        const float ao = glm::clamp(0.65f + center * 0.35f, 0.2f, 1.0f);
        const int index = (y * source.width + x) * 4;
        (*bytes)[index] = static_cast<std::uint8_t>(ao * 255.0f);
        (*bytes)[index + 1] = static_cast<std::uint8_t>(roughness * 255.0f);
        (*bytes)[index + 2] = static_cast<std::uint8_t>(metallic * 255.0f);
        (*bytes)[index + 3] = 255u;
    }
    auto texture = makeTexture(name, source.width, source.height, false,
        std::move(*bytes), TextureSemantic::ORM, TextureOrigin::Generated);
    texture->generated = true;
    return texture;
}

std::shared_ptr<Texture> generateHeightTexture(
    const Texture& source,
    const std::string& name
) {
    auto bytes = makeGeneratedBuffer(source);
    if (!bytes.has_value()) return nullptr;
    for (int y = 0; y < source.height; ++y) for (int x = 0; x < source.width; ++x) {
        const std::uint8_t value = static_cast<std::uint8_t>(glm::clamp(
            luminance(sampleRgba(source.bytes, source.width, x, y)),
            0.0f, 1.0f) * 255.0f);
        const int index = (y * source.width + x) * 4;
        (*bytes)[index] = value; (*bytes)[index + 1] = value;
        (*bytes)[index + 2] = value; (*bytes)[index + 3] = 255u;
    }
    auto texture = makeTexture(name, source.width, source.height, false,
        std::move(*bytes), TextureSemantic::Height, TextureOrigin::Generated);
    texture->generated = true;
    return texture;
}

}  // namespace render
