#include "SceneOverlayRenderer.hpp"

#include <array>
#include <cstddef>
#include <cmath>
#include <memory>
#include <string>

#include <SDL.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "render/resources/Material.hpp"

namespace {
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

void addQuad(const std::array<Vertex, 4>& vertices, render::Mesh& mesh) {
    const unsigned int base = static_cast<unsigned int>(mesh.positions.size());
    if (mesh.uvSets.size() < 2u) {
        mesh.uvSets.resize(2u);
    }
    for (const Vertex& vertex : vertices) {
        mesh.positions.push_back(vertex.position);
        mesh.normals.push_back(vertex.normal);
        mesh.colors.push_back(vertex.color);
        mesh.uvSets[0].push_back(vertex.uv0);
        mesh.uvSets[1].push_back(vertex.uv1);
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
}

void buildGroundIndicatorRing(render::Mesh& mesh) {
    constexpr int kSegments = 64;
    constexpr float kInnerRadius = 0.78f;
    constexpr float kTwoPi = 6.283185307f;
    mesh = render::Mesh{};
    mesh.uvSets.resize(2u);

    for (int segment = 0; segment <= kSegments; ++segment) {
        const float angle = kTwoPi * static_cast<float>(segment) / static_cast<float>(kSegments);
        const glm::vec2 direction(std::cos(angle), std::sin(angle));
        for (float radius : {1.0f, kInnerRadius}) {
            mesh.positions.emplace_back(direction.x * radius, 0.0f, direction.y * radius);
            mesh.normals.emplace_back(0.0f, 1.0f, 0.0f);
            mesh.colors.emplace_back(1.0f);
            mesh.uvSets[0].emplace_back(direction * radius * 0.5f + 0.5f);
            mesh.uvSets[1].emplace_back(0.0f);
        }
    }

    for (int segment = 0; segment < kSegments; ++segment) {
        const unsigned int outer = static_cast<unsigned int>(segment * 2);
        const unsigned int inner = outer + 1u;
        const unsigned int nextOuter = outer + 2u;
        const unsigned int nextInner = outer + 3u;
        mesh.indices.insert(mesh.indices.end(), {
            outer, inner, nextOuter,
            nextOuter, inner, nextInner
        });
    }
}

std::string assetRootPath(const char* subdirectory) {
    char* basePath = SDL_GetBasePath();
    std::string root = basePath != nullptr ? basePath : "";
    if (basePath != nullptr) {
        SDL_free(basePath);
    }
    return root + subdirectory;
}

GLuint uploadOverlayTexture(const std::shared_ptr<render::Texture>& texture) {
    if (!texture || !texture->valid()) {
        return 0;
    }
    GLuint handle = 0;
    glGenTextures(1, &handle);
    glBindTexture(GL_TEXTURE_2D, handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texture->width, texture->height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, texture->bytes.data());
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

bool SceneOverlayRenderer::init(const std::string& shaderRoot) {
    if (!debugColorShader_.buildFromFiles(shaderRoot + "debug_color.vert", shaderRoot + "debug_color.frag")) {
        return false;
    }

    debugMvpLocation_ = debugColorShader_.uniformLocation("uMVP");
    debugColorLocation_ = debugColorShader_.uniformLocation("uColor");
    return buildVolumeMeshes() &&
        buildDebugMeshes() &&
        buildGroundIndicatorResources() &&
        buildLightIconResources(shaderRoot);
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

bool SceneOverlayRenderer::buildGroundIndicatorResources() {
    Mesh ring{};
    buildGroundIndicatorRing(ring);
    return groundIndicatorRing_.upload(ring);
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

}  // namespace render
