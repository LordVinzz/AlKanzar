#include "Camera.hpp"

#include <algorithm>
#include <cmath>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

namespace {

constexpr float kIsoAngleX = 35.264f;
constexpr float kIsoAngleY = 45.0f;
constexpr float kBaseOrthoSize = 10.0f;
constexpr float kNearPlane = 0.1f;
constexpr float kFarPlane = 100.0f;
constexpr float kOrbitSpeedDegPerSecond = 40.0f;
constexpr float kFreeCameraFieldOfViewDeg = 60.0f;
constexpr float kFreeCameraFarPlane = 1000.0f;
constexpr float kFreeCameraMouseSensitivity = 0.15f;
constexpr float kFreeCameraSprintMultiplier = 3.0f;

glm::vec3 freeCameraForward(float yawDeg, float pitchDeg) {
    const float yaw = glm::radians(yawDeg);
    const float pitch = glm::radians(pitchDeg);
    return glm::normalize(glm::vec3(
        std::sin(yaw) * std::cos(pitch),
        std::sin(pitch),
        -std::cos(yaw) * std::cos(pitch)
    ));
}

glm::mat4 legacyCameraView(const core::CameraState& camera) {
    const float yawDeg = camera.orbitEnabled
        ? camera.orbitYawDeg
        : kIsoAngleY;
    const float viewPanX = camera.orbitEnabled ? 0.0f : camera.panX;
    const float viewPanY = camera.orbitEnabled ? 0.0f : camera.panY;
    const glm::mat4 rotateX = glm::rotate(
        glm::mat4(1.0f),
        glm::radians(kIsoAngleX),
        glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::mat4 rotateY = glm::rotate(
        glm::mat4(1.0f),
        glm::radians(-yawDeg),
        glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 translate = glm::translate(
        glm::mat4(1.0f),
        glm::vec3(-viewPanX, -viewPanY, -camera.cameraDistance));
    return translate * rotateX * rotateY;
}

}  // namespace

namespace core {

render::CameraMatrices computeCameraMatrices(const CameraState& camera, int width, int height) {
    const float aspect = static_cast<float>(width) / static_cast<float>(height > 0 ? height : 1);

    render::CameraMatrices matrices{};
    if (camera.freeCameraEnabled) {
        matrices.projection = glm::perspective(
            glm::radians(kFreeCameraFieldOfViewDeg),
            aspect,
            kNearPlane,
            kFreeCameraFarPlane
        );
        matrices.invProjection = glm::inverse(matrices.projection);
        const glm::vec3 forward = freeCameraForward(
            camera.freeYawDeg,
            camera.freePitchDeg);
        matrices.view = glm::lookAt(
            camera.freePosition,
            camera.freePosition + forward,
            glm::vec3(0.0f, 1.0f, 0.0f)
        );
        return matrices;
    }

    const float halfSize = kBaseOrthoSize / camera.zoom;
    matrices.projection = glm::ortho(
        -halfSize * aspect,
        halfSize * aspect,
        -halfSize,
        halfSize,
        kNearPlane,
        kFarPlane
    );
    matrices.invProjection = glm::inverse(matrices.projection);
    matrices.view = legacyCameraView(camera);
    return matrices;
}

void updateOrbitCamera(CameraState& camera, const TimeContext& timeContext) {
    if (!camera.orbitEnabled || camera.freeCameraEnabled) {
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

void setFreeCameraEnabled(CameraState& camera, bool enabled) {
    if (camera.freeCameraEnabled == enabled) {
        return;
    }
    if (enabled && !camera.freeCameraInitialized) {
        const glm::mat4 inverseView = glm::inverse(legacyCameraView(camera));
        camera.freePosition = glm::vec3(inverseView[3]);
        const glm::vec3 forward = glm::normalize(glm::vec3(
            inverseView * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        camera.freePitchDeg = glm::degrees(std::asin(std::clamp(
            forward.y,
            -1.0f,
            1.0f)));
        camera.freeYawDeg = glm::degrees(std::atan2(
            forward.x,
            -forward.z));
        camera.freeCameraInitialized = true;
    }
    camera.freeCameraEnabled = enabled;
    if (enabled) {
        camera.orbitEnabled = false;
    }
}

void rotateFreeCamera(
    CameraState& camera,
    float mouseDeltaX,
    float mouseDeltaY
) {
    if (!camera.freeCameraEnabled) {
        return;
    }
    camera.freeYawDeg = std::fmod(
        camera.freeYawDeg + mouseDeltaX * kFreeCameraMouseSensitivity,
        360.0f);
    camera.freePitchDeg = std::clamp(
        camera.freePitchDeg - mouseDeltaY * kFreeCameraMouseSensitivity,
        -89.0f,
        89.0f);
}

void updateFreeCamera(
    CameraState& camera,
    const FreeCameraInput& input,
    const TimeContext& timeContext
) {
    if (!camera.freeCameraEnabled || timeContext.deltaSeconds <= 0.0f) {
        return;
    }
    const glm::vec3 forward = freeCameraForward(
        camera.freeYawDeg,
        camera.freePitchDeg);
    const glm::vec3 right = glm::normalize(glm::cross(
        forward,
        glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 direction(0.0f);
    direction += forward * static_cast<float>(input.forward - input.backward);
    direction += right * static_cast<float>(input.right - input.left);
    direction.y += static_cast<float>(input.up - input.down);
    const float length = glm::length(direction);
    if (length <= 1.0e-6f) {
        return;
    }
    const float speed = camera.freeMoveSpeed *
        (input.sprint ? kFreeCameraSprintMultiplier : 1.0f);
    camera.freePosition += direction / length * speed *
        std::min(timeContext.deltaSeconds, 0.1f);
}

void adjustFreeCameraSpeed(CameraState& camera, int wheelDelta) {
    if (!camera.freeCameraEnabled || wheelDelta == 0) {
        return;
    }
    camera.freeMoveSpeed = std::clamp(
        camera.freeMoveSpeed * std::pow(1.15f, static_cast<float>(wheelDelta)),
        0.5f,
        100.0f);
}

}  // namespace core
