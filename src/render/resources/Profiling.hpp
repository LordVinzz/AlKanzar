#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Geometry.hpp"
#include "Material.hpp"

namespace render {

enum class TextureStorageFormat {
    R8 = 0,
    RGB8,
    RGBA8,
    RGBA16F,
    R32F,
    Depth24,
    Depth24Stencil8,
};

struct ResourceMemoryRecord {
    std::string name{};
    std::string category{};
    std::uint64_t cpuBytes{0};
    std::uint64_t gpuBytes{0};
};

struct MeshBufferMemoryEstimate {
    std::uint64_t vertexBytes{0};
    std::uint64_t indexBytes{0};

    [[nodiscard]] std::uint64_t totalBytes() const {
        return vertexBytes + indexBytes;
    }
};

[[nodiscard]] std::uint64_t textureFormatBytesPerPixel(Format format);
[[nodiscard]] std::uint64_t storageFormatBytesPerPixel(TextureStorageFormat format);
[[nodiscard]] std::uint64_t estimateTextureCpuBytes(const Texture& texture);
[[nodiscard]] std::uint64_t estimateTextureGpuBytes(const Texture& texture);
[[nodiscard]] std::uint64_t estimateTextureStorageBytes(
    int width,
    int height,
    int layers,
    TextureStorageFormat format,
    int mipLevels = 1
);
[[nodiscard]] MeshBufferMemoryEstimate estimateMeshBufferBytes(const Mesh& mesh);

}  // namespace render
