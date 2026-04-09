#pragma once

#include <optional>

#include "FrameData.hpp"
#include "render/RenderTypes.hpp"

namespace core {

class PickingSystem {
public:
    [[nodiscard]] std::optional<EntityId> pick(
        const FrameSceneData& frame,
        const render::CameraMatrices& camera,
        int viewportWidth,
        int viewportHeight,
        int mouseX,
        int mouseY
    ) const;
};

}  // namespace core
