#pragma once

#include "core/app/TimeContext.hpp"
#include "render/engine/RenderTypes.hpp"

#include <glm/vec3.hpp>

namespace core {

struct CameraState {
    float zoom{1.0f};
    float panX{0.0f};
    float panY{0.0f};
    float cameraDistance{15.0f};
    float orbitYawDeg{45.0f};
    bool orbitEnabled{false};
    glm::vec3 freePosition{0.0f};
    float freeYawDeg{0.0f};
    float freePitchDeg{0.0f};
    float freeMoveSpeed{8.0f};
    bool freeCameraEnabled{false};
    bool freeCameraInitialized{false};
};

struct FreeCameraInput {
    bool forward{false};
    bool backward{false};
    bool left{false};
    bool right{false};
    bool down{false};
    bool up{false};
    bool sprint{false};
};

render::CameraMatrices computeCameraMatrices(const CameraState& camera, int width, int height);
void updateOrbitCamera(CameraState& camera, const TimeContext& timeContext);
void setFreeCameraEnabled(CameraState& camera, bool enabled);
void rotateFreeCamera(CameraState& camera, float mouseDeltaX, float mouseDeltaY);
void updateFreeCamera(
    CameraState& camera,
    const FreeCameraInput& input,
    const TimeContext& timeContext
);
void adjustFreeCameraSpeed(CameraState& camera, int wheelDelta);

}  // namespace core
