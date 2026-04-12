#pragma once

#include <optional>

#include "core/app/FrameData.hpp"
#include "render/engine/RenderTypes.hpp"

namespace core {

struct ViewportRay {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

std::optional<ViewportRay> makeViewportRay(
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight,
    int mouseX,
    int mouseY
);

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
