#pragma once

#include "TimeContext.hpp"
#include "render/RenderTypes.hpp"

namespace core {

struct CameraState {
    float zoom{1.0f};
    float panX{0.0f};
    float panY{0.0f};
    float cameraDistance{15.0f};
    float orbitYawDeg{45.0f};
    bool orbitEnabled{false};
};

render::CameraMatrices computeCameraMatrices(const CameraState& camera, int width, int height);
void updateOrbitCamera(CameraState& camera, const TimeContext& timeContext);

}  // namespace core
