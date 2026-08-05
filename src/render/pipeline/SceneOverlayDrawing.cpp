#include "SceneOverlayRenderer.hpp"
#include "SceneOverlayMath.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/trigonometric.hpp>

namespace {
constexpr float kLightGizmoScaleMin = 0.45f;
constexpr float kLightGizmoScaleMax = 1.25f;
constexpr float kLightGizmoScaleFactor = 0.12f;
constexpr float kDirectionalDebugAnchorDistance = 7.5f;
constexpr float kDebugVolumeAlpha = 0.85f;
constexpr float kSelectionBoundsAlpha = 0.95f;
constexpr float kLightIconPixelSize = 34.0f;

struct LightIconInstanceGpu {
    glm::vec4 clipCenter{0.0f};
    float opacity{1.0f};
    glm::vec3 padding{0.0f};
};
}  // namespace

namespace render {

void SceneOverlayRenderer::drawDebugMesh(
    const MeshBuffer& mesh,
    const glm::mat4& projection,
    const glm::mat4& view,
    const glm::mat4& model,
    const glm::vec4& color,
    bool wireframe
) const {
    if (!mesh.valid() || debugColorShader_.id() == 0) {
        return;
    }

    const glm::mat4 mvp = projection * view * model;
    glUniformMatrix4fv(debugMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform4fv(debugColorLocation_, 1, glm::value_ptr(color));
    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    mesh.draw();
    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}

void SceneOverlayRenderer::drawLightIconBatch(
    const detail::OverlayIconBatch& batch,
    GLuint textureHandle,
    int width,
    int height
) const {
    if (batch.empty() ||
        lightIconShader_.id() == 0 ||
        !lightIconQuad_.valid() ||
        lightIconInstanceVbo_ == 0 ||
        textureHandle == 0 ||
        width <= 0 ||
        height <= 0) {
        return;
    }

    std::vector<LightIconInstanceGpu> instances{};
    instances.reserve(batch.clipCenters.size());
    for (const glm::vec4& clipCenter : batch.clipCenters) {
        instances.push_back(LightIconInstanceGpu{clipCenter, batch.opacity});
    }

    glBindBuffer(GL_ARRAY_BUFFER, lightIconInstanceVbo_);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(instances.size() * sizeof(LightIconInstanceGpu)),
        instances.data(),
        GL_DYNAMIC_DRAW
    );
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    const glm::vec2 sizeNdc(
        (kLightIconPixelSize * 2.0f) / static_cast<float>(width),
        (kLightIconPixelSize * 2.0f) / static_cast<float>(height)
    );

    lightIconShader_.use();
    glUniform2fv(lightIconSizeLocation_, 1, glm::value_ptr(sizeNdc));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureHandle);
    lightIconQuad_.drawInstanced(static_cast<GLsizei>(instances.size()));
    glBindTexture(GL_TEXTURE_2D, 0);
}

void SceneOverlayRenderer::drawSkeletonLines(
    const std::vector<glm::vec3>& jointWorldPositions,
    const std::vector<int>& parentIndices,
    const glm::mat4& projection,
    const glm::mat4& view
) const {
    if (jointWorldPositions.empty() || parentIndices.empty() || debugColorShader_.id() == 0) {
        return;
    }

    std::vector<glm::vec3> lineVertices{};
    lineVertices.reserve(jointWorldPositions.size() * 2u);
    const std::size_t jointCount = std::min(jointWorldPositions.size(), parentIndices.size());
    for (std::size_t jointIndex = 0; jointIndex < jointCount; ++jointIndex) {
        const int parentIndex = parentIndices[jointIndex];
        if (parentIndex < 0 || parentIndex >= static_cast<int>(jointCount)) {
            continue;
        }
        lineVertices.push_back(jointWorldPositions[static_cast<std::size_t>(parentIndex)]);
        lineVertices.push_back(jointWorldPositions[jointIndex]);
    }

    if (lineVertices.empty()) {
        return;
    }

    drawLineVertices(lineVertices, projection, view, glm::vec4(0.35f, 0.95f, 0.85f, 1.0f), GL_LINES);
}

void SceneOverlayRenderer::drawLineVertices(
    const std::vector<glm::vec3>& vertices,
    const glm::mat4& projection,
    const glm::mat4& view,
    const glm::vec4& color,
    GLenum primitive
) const {
    if (vertices.empty() || debugColorShader_.id() == 0) {
        return;
    }

    if (skeletonLineVao_ == 0) {
        glGenVertexArrays(1, &skeletonLineVao_);
    }
    if (skeletonLineVbo_ == 0) {
        glGenBuffers(1, &skeletonLineVbo_);
    }

    glBindVertexArray(skeletonLineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, skeletonLineVbo_);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(vertices.size() * sizeof(glm::vec3)),
        vertices.data(),
        GL_DYNAMIC_DRAW
    );
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), reinterpret_cast<void*>(0));
    glDisableVertexAttribArray(5);
    glVertexAttrib4f(5, 1.0f, 1.0f, 1.0f, 1.0f);

    const glm::mat4 mvp = projection * view;
    debugColorShader_.use();
    glUniformMatrix4fv(debugMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform4fv(debugColorLocation_, 1, glm::value_ptr(color));
    glDrawArrays(primitive, 0, static_cast<GLsizei>(vertices.size()));

    glBindVertexArray(0);
}

void SceneOverlayRenderer::drawFilledPolygon(
    const std::vector<glm::vec3>& vertices,
    const glm::mat4& projection,
    const glm::mat4& view,
    const glm::vec4& color
) const {
    if (vertices.size() < 3u) {
        return;
    }

    std::vector<glm::vec3> triangleVertices{};
    triangleVertices.reserve((vertices.size() - 2u) * 3u);
    for (std::size_t index = 1; index + 1u < vertices.size(); ++index) {
        triangleVertices.push_back(vertices[0]);
        triangleVertices.push_back(vertices[index]);
        triangleVertices.push_back(vertices[index + 1u]);
    }
    drawLineVertices(triangleVertices, projection, view, color, GL_TRIANGLES);
}

void SceneOverlayRenderer::renderGroundIndicators(
    const RenderSceneView& scene,
    const CameraMatrices& camera
) const {
    if (scene.groundIndicators.empty() ||
        debugColorShader_.id() == 0 ||
        !groundIndicatorRing_.valid()) {
        return;
    }

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    debugColorShader_.use();
    for (const RenderGroundIndicatorView& indicator : scene.groundIndicators) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), indicator.center);
        model = glm::scale(model, glm::vec3(std::max(indicator.radius, 0.01f), 1.0f, std::max(indicator.radius, 0.01f)));
        drawDebugMesh(
            groundIndicatorRing_,
            camera.projection,
            camera.view,
            model,
            indicator.color,
            false
        );
    }

    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

void SceneOverlayRenderer::renderOverlays(
    const RenderSceneView& scene,
    const RenderLightPipeline::FrameState& lights,
    const CameraMatrices& camera,
    const RenderFrameOptions& options,
    int width,
    int height,
    const glm::vec3& directionalLightDirection
) const {
    const detail::OverlayWork work = detail::buildOverlayWork(scene, lights, camera, options);
    const bool drawColliderDebug = options.editorEnabled &&
        debugColorShader_.id() != 0 &&
        selectionBox_.valid() &&
        lightSphere_.valid() &&
        !scene.colliderDebug.empty();
    const bool drawNavigationOverlay = options.showNavMeshOverlay &&
        (!scene.navigation.polygons.empty() ||
         !scene.navigation.links.empty() ||
         !scene.navigation.path.empty() ||
         scene.navigation.destination.has_value() ||
         !scene.navigation.captureVertices.empty());
    const bool drawNavigationWireframe = options.showNavMeshPolygonWireframe &&
        !scene.navigation.polygons.empty();
    const bool drawNavigation = drawNavigationOverlay || drawNavigationWireframe;
    if (!work.hasWork() && !drawColliderDebug && !drawNavigation) {
        return;
    }

    const bool drawSelection = work.drawSelection && options.editorEnabled && debugColorShader_.id() != 0 &&
        axisGizmo_.valid() && selectionBox_.valid();
    const bool drawSkeleton = work.drawSkeleton && options.editorEnabled && debugColorShader_.id() != 0;
    const bool drawIcons = options.editorEnabled && lightIconShader_.id() != 0 && lightIconQuad_.valid() && lightIconInstanceVbo_ != 0 &&
        std::any_of(work.iconBatches.begin(), work.iconBatches.end(), [](const detail::OverlayIconBatch& batch) {
            return !batch.empty();
        });
    const bool drawLightDebug = debugColorShader_.id() != 0 && axisGizmo_.valid() &&
        (!work.debugLightIndices.empty() || work.drawDirectionalMarker);
    if (!drawSelection && !drawSkeleton && !drawIcons && !drawLightDebug && !drawColliderDebug && !drawNavigation) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    if (drawSelection) {
        debugColorShader_.use();
        const glm::mat4 axisModel = detail::makeUnscaledSelectionAxisModel(scene.selection.transformMatrix, work.selectionAxisScale);
        drawDebugMesh(axisGizmo_, camera.projection, camera.view, axisModel, glm::vec4(1.0f), false);

        glm::mat4 boxModel = scene.selection.hasBoundsModelMatrix
            ? scene.selection.boundsModelMatrix
            : glm::scale(glm::translate(glm::mat4(1.0f), work.selectionCenter), work.selectionExtents);
        drawDebugMesh(
            selectionBox_,
            camera.projection,
            camera.view,
            boxModel,
            glm::vec4(0.98f, 0.85f, 0.30f, kSelectionBoundsAlpha),
            true
        );
    }
    if (drawSkeleton) {
        drawSkeletonLines(
            scene.selectionSkeleton.jointWorldPositions,
            scene.selectionSkeleton.parentIndices,
            camera.projection,
            camera.view
        );
    }
    if (drawColliderDebug) {
        for (const RenderColliderDebugView& collider : scene.colliderDebug) {
            const bool selected = scene.selection.entity == collider.entity;
            const glm::vec4 color = selected
                ? glm::vec4(0.95f, 0.88f, 0.32f, 1.0f)
                : glm::vec4(0.24f, 0.95f, 0.96f, 0.95f);
            if (collider.shape == core::FrameColliderShape::Box) {
                drawDebugMesh(selectionBox_, camera.projection, camera.view, collider.modelMatrix, color, true);
            } else {
                glm::mat4 model(1.0f);
                model = glm::translate(model, collider.center);
                model = glm::scale(model, glm::vec3(std::max(collider.radius, 0.01f)));
                drawDebugMesh(lightSphere_, camera.projection, camera.view, model, color, true);
            }
        }
    }
    if (drawIcons) {
        for (const detail::OverlayIconBatch& batch : work.iconBatches) {
            const GLuint textureHandle = batch.type == LightType::Spot ? spotLightIconTexture_ : pointLightIconTexture_;
            drawLightIconBatch(batch, textureHandle, width, height);
        }
    }
    if (drawLightDebug) {
        const int selectedLightIndex = scene.selection.kind == RenderSelectionKind::Light ? scene.selection.index : -1;
        debugColorShader_.use();
        for (int lightIndex : work.debugLightIndices) {
            const bool selected = lightIndex == selectedLightIndex;
            const ActiveLightDebug& light = lights.debugLights[lightIndex];
            glm::mat4 axisModel(1.0f);
            axisModel = glm::translate(axisModel, light.position);
            if (light.type == LightType::Spot) {
                axisModel *= detail::makeOrientationFromDirection(light.direction);
            }
            const float axisScale = std::clamp(light.radius * kLightGizmoScaleFactor, kLightGizmoScaleMin, kLightGizmoScaleMax);
            axisModel = glm::scale(axisModel, glm::vec3(axisScale));
            drawDebugMesh(
                axisGizmo_,
                camera.projection,
                camera.view,
                axisModel,
                selected ? glm::vec4(1.0f, 0.92f, 0.40f, 1.0f) : glm::vec4(1.0f),
                false
            );

            if (light.type == LightType::Point && lightSphere_.valid()) {
                glm::mat4 sphereModel(1.0f);
                sphereModel = glm::translate(sphereModel, light.position);
                sphereModel = glm::scale(sphereModel, glm::vec3(light.radius));
                drawDebugMesh(
                    lightSphere_,
                    camera.projection,
                    camera.view,
                    sphereModel,
                    glm::vec4(light.color, selected ? 1.0f : kDebugVolumeAlpha),
                    true
                );
            } else if (light.type == LightType::Spot && lightCone_.valid()) {
                glm::mat4 coneModel(1.0f);
                coneModel = glm::translate(coneModel, light.position);
                coneModel *= detail::makeOrientationFromDirection(light.direction);
                const float coneRadius = light.radius * std::tan(glm::radians(light.outerAngle));
                coneModel = glm::scale(coneModel, glm::vec3(coneRadius, coneRadius, light.radius));
                drawDebugMesh(
                    lightCone_,
                    camera.projection,
                    camera.view,
                    coneModel,
                    glm::vec4(light.color, selected ? 1.0f : kDebugVolumeAlpha),
                    true
                );
            }
        }
        if (work.drawDirectionalMarker) {
            const glm::vec3 dir = detail::normalizeOr(directionalLightDirection, glm::vec3(0.0f, 0.0f, -1.0f));
            glm::mat4 directionalModel(1.0f);
            directionalModel = glm::translate(directionalModel, -dir * kDirectionalDebugAnchorDistance);
            directionalModel *= detail::makeOrientationFromDirection(dir);
            directionalModel = glm::scale(directionalModel, glm::vec3(1.1f));
            drawDebugMesh(axisGizmo_, camera.projection, camera.view, directionalModel, glm::vec4(1.0f), false);
        }
    }
    if (drawNavigation) {
        if (drawNavigationOverlay) {
            for (const RenderNavPolygonView& polygon : scene.navigation.polygons) {
                drawFilledPolygon(polygon.vertices, camera.projection, camera.view, polygon.color);
            }
            for (const RenderNavLinkView& link : scene.navigation.links) {
                drawLineVertices(
                    std::vector<glm::vec3>{link.fromPoint, link.toPoint},
                    camera.projection,
                    camera.view,
                    glm::vec4(1.0f, 0.45f, 0.15f, 0.95f),
                    GL_LINES
                );
                if (!link.bidirectional) {
                    continue;
                }

                const glm::vec3 mid = (link.fromPoint + link.toPoint) * 0.5f;
                drawDebugMesh(
                    axisGizmo_,
                    camera.projection,
                    camera.view,
                    glm::scale(glm::translate(glm::mat4(1.0f), mid), glm::vec3(0.12f)),
                    glm::vec4(1.0f, 0.45f, 0.15f, 0.75f),
                    false
                );
            }
            if (!scene.navigation.path.empty()) {
                drawLineVertices(
                    scene.navigation.path,
                    camera.projection,
                    camera.view,
                    glm::vec4(0.98f, 0.92f, 0.18f, 0.98f),
                    GL_LINE_STRIP
                );
            }
            if (!scene.navigation.captureVertices.empty()) {
                drawLineVertices(
                    scene.navigation.captureVertices,
                    camera.projection,
                    camera.view,
                    glm::vec4(0.75f, 1.0f, 0.50f, 0.95f),
                    GL_LINE_STRIP
                );
            }
            if (scene.navigation.destination.has_value()) {
                drawDebugMesh(
                    selectionBox_,
                    camera.projection,
                    camera.view,
                    glm::scale(glm::translate(glm::mat4(1.0f), *scene.navigation.destination), glm::vec3(0.12f)),
                    glm::vec4(0.98f, 0.20f, 0.22f, 0.95f),
                    true
                );
            }
        }
        if (drawNavigationWireframe) {
            std::size_t edgeCount = 0u;
            for (const RenderNavPolygonView& polygon : scene.navigation.polygons) {
                if (polygon.vertices.size() >= 2u) {
                    edgeCount += polygon.vertices.size();
                }
            }

            std::vector<glm::vec3> wireframeVertices{};
            wireframeVertices.reserve(edgeCount * 2u);
            for (const RenderNavPolygonView& polygon : scene.navigation.polygons) {
                if (polygon.vertices.size() < 2u) {
                    continue;
                }
                for (std::size_t index = 0u; index < polygon.vertices.size(); ++index) {
                    wireframeVertices.push_back(polygon.vertices[index]);
                    wireframeVertices.push_back(polygon.vertices[(index + 1u) % polygon.vertices.size()]);
                }
            }
            drawLineVertices(
                wireframeVertices,
                camera.projection,
                camera.view,
                glm::vec4(1.0f, 0.05f, 0.05f, 1.0f),
                GL_LINES
            );
        }
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

}  // namespace render
