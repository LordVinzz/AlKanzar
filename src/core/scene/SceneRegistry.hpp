#pragma once

#include <filesystem>
#include <string>

#include "SceneBlueprint.hpp"

namespace core {

class SceneRegistry {
public:
    [[nodiscard]] SceneBlueprint defaultScene(std::string* error = nullptr) const;
    [[nodiscard]] SceneBlueprint deterministicTestScene(std::string* error = nullptr) const;
    [[nodiscard]] std::filesystem::path sourceSceneDirectory() const;
    [[nodiscard]] std::filesystem::path stagedSceneDirectory() const;
    [[nodiscard]] std::filesystem::path defaultSourceScenePath() const;
    [[nodiscard]] std::filesystem::path defaultStagedScenePath() const;
};

}  // namespace core
