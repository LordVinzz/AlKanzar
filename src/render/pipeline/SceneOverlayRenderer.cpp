#include "SceneOverlayRenderer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>

#include <SDL.h>
#include <glm/geometric.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/trigonometric.hpp>

#include "render/resources/Material.hpp"

namespace {

constexpr float kAxisLength = 1.0f;
constexpr float kAxisThickness = 0.035f;
constexpr float kAxisCenterHalfExtent = 0.055f;
constexpr float kLightGizmoScaleMin = 0.45f;
constexpr float kLightGizmoScaleMax = 1.25f;
constexpr float kLightGizmoScaleFactor = 0.12f;
constexpr float kDirectionalDebugAnchorDistance = 7.5f;
constexpr float kDebugVolumeAlpha = 0.85f;
constexpr float kSelectionBoundsAlpha = 0.95f;
constexpr float kSelectionAxisViewportFactor = 0.08f;
constexpr float kLightIconPixelSize = 34.0f;
constexpr float kLightIconOpacity = 0.5f;

struct LightIconInstanceGpu {
    glm::vec4 clipCenter{0.0f};
    float opacity{1.0f};
    glm::vec3 padding{0.0f};
};

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    glm::vec4 color{1.0f};
};

void pushVertex(const Vertex& vertex, render::Mesh& outMesh) {
    outMesh.positions.push_back(vertex.position);
    outMesh.normals.push_back(vertex.normal);
    outMesh.colors.push_back(vertex.color);
    if (outMesh.uvSets.size() < 2) {
        outMesh.uvSets.resize(2);
    }
    outMesh.uvSets[0].push_back(vertex.uv0);
    outMesh.uvSets[1].push_back(vertex.uv1);
}

void addQuad(const std::array<Vertex, 4>& verts, render::Mesh& outMesh) {
    const unsigned int base = static_cast<unsigned int>(outMesh.positions.size());
    for (const auto& vertex : verts) {
        pushVertex(vertex, outMesh);
    }
    outMesh.indices.insert(outMesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void addBox(
    const glm::vec3& minCorner,
    const glm::vec3& maxCorner,
    const glm::vec4& color,
    render::Mesh& outMesh
) {
    const float minX = minCorner.x;
    const float minY = minCorner.y;
    const float minZ = minCorner.z;
    const float maxX = maxCorner.x;
    const float maxY = maxCorner.y;
    const float maxZ = maxCorner.z;

    const std::array<Vertex, 4> rightFace{{
        {glm::vec3(maxX, minY, minZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
    }};
    addQuad(rightFace, outMesh);

    const std::array<Vertex, 4> leftFace{{
        {glm::vec3(minX, minY, minZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, minY, maxZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, minZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
    }};
    addQuad(leftFace, outMesh);

    const std::array<Vertex, 4> topFace{{
        {glm::vec3(minX, maxY, minZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
    }};
    addQuad(topFace, outMesh);

    const std::array<Vertex, 4> bottomFace{{
        {glm::vec3(minX, minY, minZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, minZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, minY, maxZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
    }};
    addQuad(bottomFace, outMesh);

    const std::array<Vertex, 4> frontFace{{
        {glm::vec3(minX, minY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
    }};
    addQuad(frontFace, outMesh);

    const std::array<Vertex, 4> backFace{{
        {glm::vec3(minX, minY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
    }};
    addQuad(backFace, outMesh);
}

void buildSphereMesh(int stacks, int slices, render::Mesh& outMesh) {
    outMesh = render::Mesh{};
    outMesh.uvSets.resize(2);

    const float pi = 3.1415926535f;
    const float twoPi = pi * 2.0f;
    for (int stack = 0; stack <= stacks; ++stack) {
        const float v = static_cast<float>(stack) / static_cast<float>(stacks);
        const float phi = v * pi;
        const float y = std::cos(phi);
        const float r = std::sin(phi);

        for (int slice = 0; slice <= slices; ++slice) {
            const float u = static_cast<float>(slice) / static_cast<float>(slices);
            const float theta = u * twoPi;
            const float x = r * std::cos(theta);
            const float z = r * std::sin(theta);

            Vertex vertex{};
            vertex.position = glm::vec3(x, y, z);
            vertex.normal = glm::vec3(x, y, z);
            vertex.uv0 = glm::vec2(u, v);
            pushVertex(vertex, outMesh);
        }
    }

    const int stride = slices + 1;
    for (int stack = 0; stack < stacks; ++stack) {
        for (int slice = 0; slice < slices; ++slice) {
            const unsigned int a = static_cast<unsigned int>(stack * stride + slice);
            const unsigned int b = static_cast<unsigned int>((stack + 1) * stride + slice);
            const unsigned int c = static_cast<unsigned int>((stack + 1) * stride + slice + 1);
            const unsigned int d = static_cast<unsigned int>(stack * stride + slice + 1);
            outMesh.indices.insert(outMesh.indices.end(), {a, b, c, a, c, d});
        }
    }
}

void buildConeMesh(int slices, render::Mesh& outMesh) {
    outMesh = render::Mesh{};
    outMesh.uvSets.resize(2);

    const float twoPi = 6.283185307f;
    Vertex apex{};
    apex.position = glm::vec3(0.0f, 0.0f, 0.0f);
    apex.normal = glm::vec3(0.0f, 0.0f, -1.0f);
    apex.uv0 = glm::vec2(0.5f, 0.0f);
    pushVertex(apex, outMesh);

    for (int i = 0; i <= slices; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(slices);
        const float theta = u * twoPi;
        const float x = std::cos(theta);
        const float y = std::sin(theta);

        Vertex vertex{};
        vertex.position = glm::vec3(x, y, 1.0f);
        vertex.normal = glm::vec3(x, y, 0.0f);
        vertex.uv0 = glm::vec2(u, 1.0f);
        pushVertex(vertex, outMesh);
    }

    const unsigned int baseCenterIndex = static_cast<unsigned int>(outMesh.positions.size());
    Vertex center{};
    center.position = glm::vec3(0.0f, 0.0f, 1.0f);
    center.normal = glm::vec3(0.0f, 0.0f, 1.0f);
    center.uv0 = glm::vec2(0.5f, 0.5f);
    pushVertex(center, outMesh);

    for (int i = 0; i < slices; ++i) {
        const unsigned int i0 = 1u + static_cast<unsigned int>(i);
        const unsigned int i1 = 1u + static_cast<unsigned int>(i + 1);
        outMesh.indices.insert(outMesh.indices.end(), {0u, i1, i0});
    }

    for (int i = 0; i < slices; ++i) {
        const unsigned int i0 = 1u + static_cast<unsigned int>(i);
        const unsigned int i1 = 1u + static_cast<unsigned int>(i + 1);
        outMesh.indices.insert(outMesh.indices.end(), {baseCenterIndex, i0, i1});
    }
}

glm::vec3 stableUp(const glm::vec3& dir) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(dir, up)) > 0.95f) {
        return glm::vec3(0.0f, 0.0f, 1.0f);
    }
    return up;
}

glm::mat4 makeOrientationFromDirection(const glm::vec3& direction) {
    if (glm::dot(direction, direction) < 1.0e-6f) {
        return glm::mat4(1.0f);
    }

    const glm::vec3 forward = glm::normalize(direction);
    const glm::vec3 up = stableUp(forward);
    const glm::vec3 right = glm::normalize(glm::cross(up, forward));
    const glm::vec3 actualUp = glm::normalize(glm::cross(forward, right));

    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(right, 0.0f);
    basis[1] = glm::vec4(actualUp, 0.0f);
    basis[2] = glm::vec4(forward, 0.0f);
    return basis;
}

float selectionAxisScaleFromCamera(const glm::mat4& projection) {
    const float verticalHalfSpan = projection[1][1] != 0.0f ? 1.0f / projection[1][1] : 1.0f;
    return verticalHalfSpan * kSelectionAxisViewportFactor;
}

glm::vec3 normalizeOr(const glm::vec3& value, const glm::vec3& fallback) {
    const float length = glm::length(value);
    if (length <= 1.0e-6f) {
        return fallback;
    }
    return value / length;
}

glm::mat4 makeUnscaledSelectionAxisModel(const glm::mat4& transformMatrix, float axisScale) {
    glm::vec3 translation = glm::vec3(transformMatrix[3]);
    const glm::vec3 originalX = glm::vec3(transformMatrix[0]);
    const glm::vec3 originalY = glm::vec3(transformMatrix[1]);
    const glm::vec3 originalZ = glm::vec3(transformMatrix[2]);

    glm::vec3 xAxis = normalizeOr(originalX, glm::vec3(1.0f, 0.0f, 0.0f));
    glm::vec3 yAxis = originalY - xAxis * glm::dot(originalY, xAxis);
    yAxis = normalizeOr(yAxis, glm::vec3(0.0f, 1.0f, 0.0f));

    glm::vec3 zAxis = glm::cross(xAxis, yAxis);
    if (glm::dot(zAxis, zAxis) <= 1.0e-6f) {
        zAxis = normalizeOr(originalZ, glm::vec3(0.0f, 0.0f, 1.0f));
    } else {
        zAxis = glm::normalize(zAxis);
    }
    if (glm::dot(zAxis, originalZ) < 0.0f) {
        zAxis = -zAxis;
    }
    yAxis = normalizeOr(glm::cross(zAxis, xAxis), glm::vec3(0.0f, 1.0f, 0.0f));

    glm::mat4 axisModel(1.0f);
    axisModel[0] = glm::vec4(xAxis, 0.0f);
    axisModel[1] = glm::vec4(yAxis, 0.0f);
    axisModel[2] = glm::vec4(zAxis, 0.0f);
    axisModel[3] = glm::vec4(translation, 1.0f);

    return glm::scale(axisModel, glm::vec3(axisScale));
}

std::string assetRootPath(const char* subdir) {
    char* basePath = SDL_GetBasePath();
    std::string root = basePath ? basePath : "";
    if (basePath) {
        SDL_free(basePath);
    }
    return root + subdir;
}

GLuint uploadOverlayTexture(const std::shared_ptr<render::Texture>& texture) {
    if (!texture || !texture->valid()) {
        return 0;
    }

    GLuint handle = 0;
    glGenTextures(1, &handle);
    glBindTexture(GL_TEXTURE_2D, handle);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        texture->width,
        texture->height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        texture->bytes.data()
    );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return handle;
}

render::detail::OverlayWork makeEmptyOverlayWork() {
    render::detail::OverlayWork work{};
    work.iconBatches[0].kind = render::detail::OverlayIconBatchKind::UnselectedPoint;
    work.iconBatches[0].type = render::LightType::Point;
    work.iconBatches[0].selected = false;
    work.iconBatches[0].opacity = kLightIconOpacity;

    work.iconBatches[1].kind = render::detail::OverlayIconBatchKind::UnselectedSpot;
    work.iconBatches[1].type = render::LightType::Spot;
    work.iconBatches[1].selected = false;
    work.iconBatches[1].opacity = kLightIconOpacity;

    work.iconBatches[2].kind = render::detail::OverlayIconBatchKind::SelectedPoint;
    work.iconBatches[2].type = render::LightType::Point;
    work.iconBatches[2].selected = true;
    work.iconBatches[2].opacity = 1.0f;

    work.iconBatches[3].kind = render::detail::OverlayIconBatchKind::SelectedSpot;
    work.iconBatches[3].type = render::LightType::Spot;
    work.iconBatches[3].selected = true;
    work.iconBatches[3].opacity = 1.0f;
    return work;
}

std::size_t lightIconBatchIndex(render::LightType type, bool selected) {
    if (type == render::LightType::Spot) {
        return selected ? 3u : 1u;
    }
    return selected ? 2u : 0u;
}

bool clipCenterVisible(const glm::vec4& clipCenter) {
    if (clipCenter.w <= 0.0f) {
        return false;
    }

    const glm::vec3 ndc = glm::vec3(clipCenter) / clipCenter.w;
    return ndc.x >= -1.0f && ndc.x <= 1.0f &&
        ndc.y >= -1.0f && ndc.y <= 1.0f &&
        ndc.z >= -1.0f && ndc.z <= 1.0f;
}

}  // namespace

namespace render {

namespace detail {

bool OverlayWork::hasWork() const {
    return drawSelection || drawSkeleton || drawDirectionalMarker || !debugLightIndices.empty() ||
        std::any_of(iconBatches.begin(), iconBatches.end(), [](const OverlayIconBatch& batch) {
            return !batch.empty();
        });
}

OverlayWork buildOverlayWork(
    const RenderSceneView& scene,
    const RenderLightPipeline::FrameState& lights,
    const CameraMatrices& camera,
    const RenderFrameOptions& options
) {
    OverlayWork work = makeEmptyOverlayWork();

    if (options.editorEnabled) {
        work.drawSkeleton = scene.selectionSkeleton.showOverlay &&
            !scene.selectionSkeleton.jointWorldPositions.empty() &&
            !scene.selectionSkeleton.parentIndices.empty();

        work.drawSelection = scene.selection.kind != RenderSelectionKind::None &&
            scene.selection.kind != RenderSelectionKind::Light &&
            scene.selection.hasWorldBounds;
        if (work.drawSelection &&
            scene.selection.kind == RenderSelectionKind::Renderable &&
            (scene.selection.index < 0 || scene.selection.index >= static_cast<int>(scene.objects.size()))) {
            work.drawSelection = false;
        }
        if (work.drawSelection) {
            work.selectionCenter = (scene.selection.worldBounds.min + scene.selection.worldBounds.max) * 0.5f;
            work.selectionExtents = glm::max((scene.selection.worldBounds.max - scene.selection.worldBounds.min) * 0.5f, glm::vec3(0.01f));
            work.selectionAxisScale = selectionAxisScaleFromCamera(camera.projection);
        }

        const glm::mat4 viewProjection = camera.projection * camera.view;
        const int selectedLightIndex = scene.selection.kind == RenderSelectionKind::Light ? scene.selection.index : -1;
        for (std::size_t lightIndex = 0; lightIndex < scene.lights.size(); ++lightIndex) {
            const core::FrameLight& light = scene.lights[lightIndex];
            const glm::vec4 clipCenter = viewProjection * glm::vec4(light.position, 1.0f);
            if (!clipCenterVisible(clipCenter)) {
                continue;
            }

            const bool selected = static_cast<int>(lightIndex) == selectedLightIndex;
            work.iconBatches[lightIconBatchIndex(light.type, selected)].clipCenters.push_back(clipCenter);
        }
    }

    const int selectedLightIndex = scene.selection.kind == RenderSelectionKind::Light ? scene.selection.index : -1;
    const bool drawLightDebug = options.showLightDebug || (options.editorEnabled && selectedLightIndex >= 0);
    if (drawLightDebug) {
        for (int lightIndex : lights.activeLightIndices) {
            if (lightIndex < 0 || lightIndex >= static_cast<int>(lights.debugLights.size())) {
                continue;
            }
            const bool selected = lightIndex == selectedLightIndex;
            if (!options.showLightDebug && !selected) {
                continue;
            }
            work.debugLightIndices.push_back(lightIndex);
        }
        work.drawDirectionalMarker = options.showLightDebug;
    }

    return work;
}

}  // namespace detail

SceneOverlayRenderer::~SceneOverlayRenderer() {
    destroy();
}

bool SceneOverlayRenderer::init(const std::string& shaderRoot) {
    if (!debugColorShader_.buildFromFiles(shaderRoot + "debug_color.vert", shaderRoot + "debug_color.frag")) {
        return false;
    }

    debugMvpLocation_ = debugColorShader_.uniformLocation("uMVP");
    debugColorLocation_ = debugColorShader_.uniformLocation("uColor");
    return buildVolumeMeshes() && buildDebugMeshes() && buildLightIconResources(shaderRoot);
}

void SceneOverlayRenderer::destroy() {
    if (pointLightIconTexture_ != 0) {
        glDeleteTextures(1, &pointLightIconTexture_);
        pointLightIconTexture_ = 0;
    }
    if (spotLightIconTexture_ != 0) {
        glDeleteTextures(1, &spotLightIconTexture_);
        spotLightIconTexture_ = 0;
    }
    if (lightIconInstanceVbo_ != 0) {
        glDeleteBuffers(1, &lightIconInstanceVbo_);
        lightIconInstanceVbo_ = 0;
    }
    if (skeletonLineVbo_ != 0) {
        glDeleteBuffers(1, &skeletonLineVbo_);
        skeletonLineVbo_ = 0;
    }
    if (skeletonLineVao_ != 0) {
        glDeleteVertexArrays(1, &skeletonLineVao_);
        skeletonLineVao_ = 0;
    }
    debugMvpLocation_ = -1;
    debugColorLocation_ = -1;
    lightIconSizeLocation_ = -1;
}

bool SceneOverlayRenderer::buildVolumeMeshes() {
    Mesh sphereMesh{};
    buildSphereMesh(16, 24, sphereMesh);

    Mesh coneMesh{};
    buildConeMesh(24, coneMesh);

    return lightSphere_.upload(sphereMesh) && lightCone_.upload(coneMesh);
}

bool SceneOverlayRenderer::buildDebugMeshes() {
    Mesh axisMesh{};
    addBox(
        glm::vec3(-kAxisCenterHalfExtent),
        glm::vec3(kAxisCenterHalfExtent),
        glm::vec4(0.95f, 0.95f, 0.95f, 1.0f),
        axisMesh
    );
    addBox(
        glm::vec3(0.0f, -kAxisThickness, -kAxisThickness),
        glm::vec3(kAxisLength, kAxisThickness, kAxisThickness),
        glm::vec4(0.95f, 0.20f, 0.18f, 1.0f),
        axisMesh
    );
    addBox(
        glm::vec3(-kAxisThickness, 0.0f, -kAxisThickness),
        glm::vec3(kAxisThickness, kAxisLength, kAxisThickness),
        glm::vec4(0.20f, 0.92f, 0.24f, 1.0f),
        axisMesh
    );
    addBox(
        glm::vec3(-kAxisThickness, -kAxisThickness, 0.0f),
        glm::vec3(kAxisThickness, kAxisThickness, kAxisLength),
        glm::vec4(0.18f, 0.48f, 0.96f, 1.0f),
        axisMesh
    );

    Mesh selectionMesh{};
    addBox(
        glm::vec3(-1.0f, -1.0f, -1.0f),
        glm::vec3(1.0f, 1.0f, 1.0f),
        glm::vec4(1.0f),
        selectionMesh
    );

    return axisGizmo_.upload(axisMesh) && selectionBox_.upload(selectionMesh);
}

bool SceneOverlayRenderer::buildLightIconResources(const std::string& shaderRoot) {
    if (!lightIconShader_.buildFromFiles(shaderRoot + "light_icon.vert", shaderRoot + "light_icon.frag")) {
        return false;
    }

    lightIconSizeLocation_ = lightIconShader_.uniformLocation("uSizeNdc");

    Mesh iconQuad{};
    addQuad(
        {{
            {glm::vec3(-0.5f, -0.5f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
            {glm::vec3(-0.5f, 0.5f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
            {glm::vec3(0.5f, 0.5f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
            {glm::vec3(0.5f, -0.5f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
        }},
        iconQuad
    );

    lightIconShader_.use();
    glUniform1i(lightIconShader_.uniformLocation("uIconTexture"), 0);
    if (!lightIconQuad_.upload(iconQuad) || !loadLightIconTextures()) {
        return false;
    }

    if (lightIconInstanceVbo_ == 0) {
        glGenBuffers(1, &lightIconInstanceVbo_);
    }
    if (lightIconInstanceVbo_ == 0) {
        return false;
    }

    lightIconQuad_.bind();
    glBindBuffer(GL_ARRAY_BUFFER, lightIconInstanceVbo_);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(
        8,
        4,
        GL_FLOAT,
        GL_FALSE,
        static_cast<GLsizei>(sizeof(LightIconInstanceGpu)),
        reinterpret_cast<void*>(offsetof(LightIconInstanceGpu, clipCenter))
    );
    glVertexAttribDivisor(8, 1);
    glEnableVertexAttribArray(9);
    glVertexAttribPointer(
        9,
        1,
        GL_FLOAT,
        GL_FALSE,
        static_cast<GLsizei>(sizeof(LightIconInstanceGpu)),
        reinterpret_cast<void*>(offsetof(LightIconInstanceGpu, opacity))
    );
    glVertexAttribDivisor(9, 1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    MeshBuffer::unbind();
    return true;
}

bool SceneOverlayRenderer::loadLightIconTextures() {
    const std::string textureRoot = assetRootPath("textures/engine/");
    pointLightIconTexture_ = uploadOverlayTexture(loadTextureFromFile(
        textureRoot + "point_light_gizmo.png",
        "PointLightGizmo",
        false,
        TextureSemantic::Generic
    ));
    spotLightIconTexture_ = uploadOverlayTexture(loadTextureFromFile(
        textureRoot + "spot_light_gizmo.png",
        "SpotLightGizmo",
        false,
        TextureSemantic::Generic
    ));
    return pointLightIconTexture_ != 0 && spotLightIconTexture_ != 0;
}

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
    const bool drawNavigation = options.showNavMeshOverlay &&
        (!scene.navigation.polygons.empty() ||
         !scene.navigation.links.empty() ||
         !scene.navigation.path.empty() ||
         scene.navigation.destination.has_value() ||
         !scene.navigation.captureVertices.empty());
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
        const glm::mat4 axisModel = makeUnscaledSelectionAxisModel(scene.selection.transformMatrix, work.selectionAxisScale);
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
                axisModel *= makeOrientationFromDirection(light.direction);
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
                coneModel *= makeOrientationFromDirection(light.direction);
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
            const glm::vec3 dir = normalizeOr(directionalLightDirection, glm::vec3(0.0f, 0.0f, -1.0f));
            glm::mat4 directionalModel(1.0f);
            directionalModel = glm::translate(directionalModel, -dir * kDirectionalDebugAnchorDistance);
            directionalModel *= makeOrientationFromDirection(dir);
            directionalModel = glm::scale(directionalModel, glm::vec3(1.1f));
            drawDebugMesh(axisGizmo_, camera.projection, camera.view, directionalModel, glm::vec4(1.0f), false);
        }
    }
    if (drawNavigation) {
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
            if (link.bidirectional) {
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
        }
        if (!scene.navigation.path.empty()) {
            std::vector<glm::vec3> pathVertices = scene.navigation.path;
            drawLineVertices(pathVertices, camera.projection, camera.view, glm::vec4(0.98f, 0.92f, 0.18f, 0.98f), GL_LINE_STRIP);
        }
        if (!scene.navigation.captureVertices.empty()) {
            std::vector<glm::vec3> captureLoop = scene.navigation.captureVertices;
            drawLineVertices(captureLoop, camera.projection, camera.view, glm::vec4(0.75f, 1.0f, 0.50f, 0.95f), GL_LINE_STRIP);
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

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

}  // namespace render
