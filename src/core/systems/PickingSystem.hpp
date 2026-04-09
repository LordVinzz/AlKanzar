#pragma once

#include <optional>

#include "core/app/FrameData.hpp"
#include "render/engine/RenderTypes.hpp"

namespace core {

class PickingSystem {
public:
    [[nodiscard]] std::optional<EntityId> pick(
        const FrameSceneData& frame,
        const render::CameraMatrices& camera,
        int viewportWidth,
        int viewportHeight,
        int mouseX,
        int mouseY,
        bool includeLights = true
    ) const;
};

}  // namespace core
