#include "Camera.hpp"

#include <cmath>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace {

constexpr float kIsoAngleX = 35.264f;
constexpr float kIsoAngleY = 45.0f;
constexpr float kBaseOrthoSize = 10.0f;
constexpr float kNearPlane = 0.1f;
constexpr float kFarPlane = 100.0f;
constexpr float kOrbitSpeedDegPerSecond = 40.0f;

}  // namespace

namespace core {

render::CameraMatrices computeCameraMatrices(const CameraState& camera, int width, int height) {
    const float aspect = static_cast<float>(width) / static_cast<float>(height > 0 ? height : 1);
    const float halfSize = kBaseOrthoSize / camera.zoom;
    const float yawDeg = camera.orbitEnabled ? camera.orbitYawDeg : kIsoAngleY;
    const float viewPanX = camera.orbitEnabled ? 0.0f : camera.panX;
    const float viewPanY = camera.orbitEnabled ? 0.0f : camera.panY;

    render::CameraMatrices matrices{};
    matrices.projection = glm::ortho(
        -halfSize * aspect,
        halfSize * aspect,
        -halfSize,
        halfSize,
        kNearPlane,
        kFarPlane
    );
    matrices.invProjection = glm::inverse(matrices.projection);

    const glm::mat4 rotateX = glm::rotate(glm::mat4(1.0f), glm::radians(kIsoAngleX), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::mat4 rotateY = glm::rotate(glm::mat4(1.0f), glm::radians(-yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 translate = glm::translate(glm::mat4(1.0f), glm::vec3(-viewPanX, -viewPanY, -camera.cameraDistance));
    matrices.view = translate * rotateX * rotateY;
    return matrices;
}

void updateOrbitCamera(CameraState& camera, const TimeContext& timeContext) {
    if (!camera.orbitEnabled) {
        return;
    }

    camera.orbitYawDeg = std::fmod(
        camera.orbitYawDeg + kOrbitSpeedDegPerSecond * timeContext.deltaSeconds,
        360.0f
    );
    if (camera.orbitYawDeg < 0.0f) {
        camera.orbitYawDeg += 360.0f;
    }
}

}  // namespace core
