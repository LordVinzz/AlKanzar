#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "SceneBlueprint.hpp"

namespace core {

inline constexpr std::uint32_t kLegacySceneAssetVersion = 1u;
inline constexpr std::uint32_t kSceneAssetVersion = 2u;
inline constexpr std::string_view kSceneContentType = "SCN";

[[nodiscard]] bool parseSceneAsset(
    std::string_view bytes,
    SceneBlueprint& outBlueprint,
    std::string* error = nullptr,
    std::string_view chunkName = "scene"
);

[[nodiscard]] bool loadSceneAsset(
    const std::filesystem::path& path,
    SceneBlueprint& outBlueprint,
    std::string* error = nullptr
);

[[nodiscard]] bool serializeSceneAsset(
    const SceneBlueprint& blueprint,
    std::string& outBytes,
    std::string* error = nullptr
);

[[nodiscard]] bool saveSceneAsset(
    const std::filesystem::path& path,
    const SceneBlueprint& blueprint,
    std::string* error = nullptr
);

}  // namespace core
