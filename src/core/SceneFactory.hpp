#pragma once

#include "MaterialLibrary.hpp"
#include "SceneBlueprint.hpp"
#include "World.hpp"

namespace render {
class RenderEngine;
}

namespace core {

class SceneFactory {
public:
    bool buildScene(
        const SceneBlueprint& blueprint,
        World& world,
        MaterialLibrary& materials,
        render::RenderEngine& renderer
    ) const;
};

}  // namespace core
