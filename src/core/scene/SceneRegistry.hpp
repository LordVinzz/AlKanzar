#pragma once

#include <string>

#include "SceneBlueprint.hpp"

namespace core {

class SceneRegistry {
public:
    [[nodiscard]] SceneBlueprint defaultScene(std::string* error = nullptr) const;
    [[nodiscard]] SceneBlueprint deterministicTestScene(std::string* error = nullptr) const;
};

}  // namespace core
