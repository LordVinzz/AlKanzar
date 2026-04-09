#include "Profiling.hpp"

#include <algorithm>
#include <cmath>

namespace render {

namespace {

std::uint64_t sumMipPixels(int width, int height, int layers, int mipLevels) {
    if (width <= 0 || height <= 0 || layers <= 0 || mipLevels <= 0) {
        return 0;
    }

    std::uint64_t totalPixels = 0;
    for (int level = 0; level < mipLevels; ++level) {
        const int levelWidth = std::max(width >> level, 1);
        const int levelHeight = std::max(height >> level, 1);
        totalPixels += static_cast<std::uint64_t>(levelWidth) *
            static_cast<std::uint64_t>(levelHeight) *
            static_cast<std::uint64_t>(layers);
    }
    return totalPixels;
}

int fullMipChainLevels(int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const int largestDimension = std::max(width, height);
    return 1 + static_cast<int>(std::floor(std::log2(static_cast<float>(largestDimension))));
}

}  // namespace

std::uint64_t textureFormatBytesPerPixel(Format format) {
    switch (format) {
        case Format::R8:
            return 1u;
        case Format::RGB8:
            return 3u;
        case Format::RGBA16F:
            return 8u;
        case Format::RGBA8:
        case Format::Unknown:
        default:
            return 4u;
    }
}

std::uint64_t storageFormatBytesPerPixel(TextureStorageFormat format) {
    switch (format) {
        case TextureStorageFormat::R8:
            return 1u;
        case TextureStorageFormat::RGB8:
            return 3u;
        case TextureStorageFormat::RGBA16F:
            return 8u;
        case TextureStorageFormat::R32F:
        case TextureStorageFormat::RGBA8:
        case TextureStorageFormat::Depth24:
        case TextureStorageFormat::Depth24Stencil8:
        default:
            return 4u;
    }
}

std::uint64_t estimateTextureCpuBytes(const Texture& texture) {
    return static_cast<std::uint64_t>(texture.bytes.size());
}

std::uint64_t estimateTextureGpuBytes(const Texture& texture) {
    if (texture.width <= 0 || texture.height <= 0) {
        return 0;
    }

    TextureStorageFormat storageFormat = TextureStorageFormat::RGBA8;
    switch (texture.format) {
        case Format::R8:
            storageFormat = TextureStorageFormat::R8;
            break;
        case Format::RGB8:
            storageFormat = TextureStorageFormat::RGB8;
            break;
        case Format::RGBA16F:
            storageFormat = TextureStorageFormat::RGBA16F;
            break;
        case Format::RGBA8:
        case Format::Unknown:
        default:
            storageFormat = TextureStorageFormat::RGBA8;
            break;
    }

    const int mipLevels = texture.mipLevels > 1
        ? texture.mipLevels
        : std::max(fullMipChainLevels(texture.width, texture.height), 1);
    return estimateTextureStorageBytes(
        texture.width,
        texture.height,
        1,
        storageFormat,
        mipLevels
    );
}

std::uint64_t estimateTextureStorageBytes(
    int width,
    int height,
    int layers,
    TextureStorageFormat format,
    int mipLevels
) {
    return sumMipPixels(width, height, layers, mipLevels) * storageFormatBytesPerPixel(format);
}

MeshBufferMemoryEstimate estimateMeshBufferBytes(const Mesh& mesh) {
    if (mesh.empty()) {
        return {};
    }

    return MeshBufferMemoryEstimate{
        static_cast<std::uint64_t>(mesh.vertexCount()) * 18u * sizeof(float),
        static_cast<std::uint64_t>(mesh.indices.size()) * sizeof(unsigned int),
    };
}

}  // namespace render
