#pragma once

#include "SceneBlueprint.hpp"

namespace core {

class SceneRegistry {
public:
    [[nodiscard]] SceneBlueprint defaultScene() const;
};

}  // namespace core
