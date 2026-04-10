#include "SceneOverlayRenderer.hpp"

#include <algorithm>
#include <array>
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
constexpr float kSelectionAxisScaleMin = 0.55f;
constexpr float kSelectionAxisScaleMax = 1.35f;
constexpr float kSelectionScaleFactor = 0.2f;
constexpr float kLightIconPixelSize = 34.0f;
constexpr float kLightIconOpacity = 0.5f;

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

}  // namespace

namespace render {

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
    lightIconClipCenterLocation_ = -1;
    lightIconSizeLocation_ = -1;
    lightIconOpacityLocation_ = -1;
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

    lightIconClipCenterLocation_ = lightIconShader_.uniformLocation("uClipCenter");
    lightIconSizeLocation_ = lightIconShader_.uniformLocation("uSizeNdc");
    lightIconOpacityLocation_ = lightIconShader_.uniformLocation("uOpacity");

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
    return lightIconQuad_.upload(iconQuad) && loadLightIconTextures();
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

void SceneOverlayRenderer::drawLightIcon(
    const glm::vec4& clipCenter,
    GLuint textureHandle,
    float opacity,
    int width,
    int height
) const {
    if (lightIconShader_.id() == 0 || !lightIconQuad_.valid() || textureHandle == 0 || width <= 0 || height <= 0) {
        return;
    }

    const glm::vec2 sizeNdc(
        (kLightIconPixelSize * 2.0f) / static_cast<float>(width),
        (kLightIconPixelSize * 2.0f) / static_cast<float>(height)
    );

    lightIconShader_.use();
    glUniform4fv(lightIconClipCenterLocation_, 1, glm::value_ptr(clipCenter));
    glUniform2fv(lightIconSizeLocation_, 1, glm::value_ptr(sizeNdc));
    glUniform1f(lightIconOpacityLocation_, opacity);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureHandle);
    lightIconQuad_.draw();
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
        static_cast<GLsizeiptr>(lineVertices.size() * sizeof(glm::vec3)),
        lineVertices.data(),
        GL_DYNAMIC_DRAW
    );
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), reinterpret_cast<void*>(0));
    glDisableVertexAttribArray(5);
    glVertexAttrib4f(5, 1.0f, 1.0f, 1.0f, 1.0f);

    const glm::mat4 mvp = projection * view;
    debugColorShader_.use();
    glUniformMatrix4fv(debugMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform4f(debugColorLocation_, 0.35f, 0.95f, 0.85f, 1.0f);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(lineVertices.size()));

    glBindVertexArray(0);
}

void SceneOverlayRenderer::renderSelectionOverlay(
    const RenderSceneView& scene,
    const CameraMatrices& camera,
    const RenderFrameOptions& options,
    int width,
    int height
) const {
    if (!options.editorEnabled || debugColorShader_.id() == 0) {
        return;
    }

    const bool drawSkeleton = scene.selectionSkeleton.showOverlay &&
        !scene.selectionSkeleton.jointWorldPositions.empty() &&
        !scene.selectionSkeleton.parentIndices.empty();

    bool drawSelection = scene.selection.kind != RenderSelectionKind::None &&
        scene.selection.kind != RenderSelectionKind::Light &&
        scene.selection.hasWorldBounds &&
        axisGizmo_.valid() &&
        selectionBox_.valid();
    if (drawSelection &&
        scene.selection.kind == RenderSelectionKind::Renderable &&
        (scene.selection.index < 0 || scene.selection.index >= static_cast<int>(scene.objects.size()))) {
        drawSelection = false;
    }
    if (!drawSelection && !drawSkeleton) {
        return;
    }

    Bounds3 worldBounds{};
    glm::vec3 center(0.0f);
    glm::vec3 extents(0.01f);
    float axisScale = kSelectionAxisScaleMin;
    if (drawSelection) {
        worldBounds = scene.selection.worldBounds;
        center = (worldBounds.min + worldBounds.max) * 0.5f;
        extents = glm::max((worldBounds.max - worldBounds.min) * 0.5f, glm::vec3(0.01f));
        axisScale = std::clamp(
            glm::length(worldBounds.max - worldBounds.min) * kSelectionScaleFactor,
            kSelectionAxisScaleMin,
            kSelectionAxisScaleMax
        );
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    debugColorShader_.use();

    if (drawSelection) {
        glm::mat4 axisModel = scene.selection.transformMatrix;
        axisModel *= glm::scale(glm::mat4(1.0f), glm::vec3(axisScale));
        drawDebugMesh(axisGizmo_, camera.projection, camera.view, axisModel, glm::vec4(1.0f), false);

        glm::mat4 boxModel(1.0f);
        boxModel = glm::translate(boxModel, center);
        boxModel = glm::scale(boxModel, extents);
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

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

void SceneOverlayRenderer::renderLightDebugOverlay(
    const RenderSceneView& scene,
    const RenderLightPipeline::FrameState& lights,
    const CameraMatrices& camera,
    const RenderFrameOptions& options,
    int width,
    int height,
    const glm::vec3& directionalLightDirection
) const {
    int selectedLightIndex = -1;
    if (scene.selection.kind == RenderSelectionKind::Light) {
        selectedLightIndex = scene.selection.index;
    }

    if (!options.editorEnabled && !options.showLightDebug) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    if (options.editorEnabled && lightIconShader_.id() != 0 && lightIconQuad_.valid()) {
        const glm::mat4 viewProjection = camera.projection * camera.view;
        auto drawIconPass = [&](bool selectedPass) {
            for (std::size_t lightIndex = 0; lightIndex < scene.lights.size(); ++lightIndex) {
                const bool selected = static_cast<int>(lightIndex) == selectedLightIndex;
                if (selected != selectedPass) {
                    continue;
                }

                const core::FrameLight& light = scene.lights[lightIndex];
                const glm::vec4 clipCenter = viewProjection * glm::vec4(light.position, 1.0f);
                if (clipCenter.w <= 0.0f) {
                    continue;
                }

                const float ndcZ = clipCenter.z / clipCenter.w;
                if (ndcZ < -1.0f || ndcZ > 1.0f) {
                    continue;
                }

                const GLuint textureHandle = light.type == LightType::Spot ? spotLightIconTexture_ : pointLightIconTexture_;
                drawLightIcon(clipCenter, textureHandle, selected ? 1.0f : kLightIconOpacity, width, height);
            }
        };

        drawIconPass(false);
        drawIconPass(true);
    }

    if ((options.showLightDebug || selectedLightIndex >= 0) && debugColorShader_.id() != 0 && axisGizmo_.valid()) {
        debugColorShader_.use();
        for (int lightIndex : lights.activeLightIndices) {
            const bool selected = lightIndex == selectedLightIndex;
            if (!options.showLightDebug && !selected) {
                continue;
            }

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
        if (options.showLightDebug) {
            const glm::vec3 dir = glm::normalize(directionalLightDirection);
            glm::mat4 directionalModel(1.0f);
            directionalModel = glm::translate(directionalModel, -dir * kDirectionalDebugAnchorDistance);
            directionalModel *= makeOrientationFromDirection(dir);
            directionalModel = glm::scale(directionalModel, glm::vec3(1.1f));
            drawDebugMesh(axisGizmo_, camera.projection, camera.view, directionalModel, glm::vec4(1.0f), false);
        }
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

}  // namespace render
