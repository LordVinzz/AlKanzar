#pragma once

#include "SceneBlueprint.hpp"
#include "core/ecs/World.hpp"

namespace render {
class RenderEngine;
}

namespace core {

class SceneFactory {
public:
    bool buildScene(
        const SceneBlueprint& blueprint,
        World& world,
        render::RenderEngine& renderer
    ) const;
};

}  // namespace core
