#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL_opengl.h>

#include <array>
#include <initializer_list>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "MeshBuffer.hpp"
#include "ShaderProgram.hpp"

namespace render {

/**
 * Manages shadow map resources and rendering for directional/spot/point lights.
 */
class ShadowSystem {
public:
    /**
     * Maximum number of cascades supported for the directional light.
     */
    static constexpr int kMaxCascades = 4;
    /**
     * Maximum number of spot lights that can cast shadows.
     */
    static constexpr int kMaxSpotShadows = 4;
    /**
     * Maximum number of point lights that can cast shadows.
     */
    static constexpr int kMaxPointShadows = 2;

    /**
     * Descriptor for a spot light shadow request.
     */
    struct SpotShadowDesc {
        glm::vec3 position;
        glm::vec3 direction;
        float radius;
        float outerAngleDeg;
        float biasMin;
        float biasSlope;
    };

    /**
     * Descriptor for a point light shadow request.
     */
    struct PointShadowDesc {
        glm::vec3 position;
        float radius;
        float biasMin;
        float biasSlope;
    };

    /**
     * Builds shadow shaders and allocates shadow map resources.
     * @param shaderRoot Directory containing shadow shaders.
     * @return true if initialization succeeds.
     */
    bool init(const std::string& shaderRoot);
    /**
     * Releases shadow map resources.
     */
    void destroy();

    /**
     * Updates directional cascade matrices from the current camera and light.
     * @param view Camera view matrix.
     * @param proj Camera projection matrix.
     * @param lightDirWorld Directional light direction in world space.
     * @param nearPlane Camera near plane distance.
     * @param farPlane Camera far plane distance.
     */
    void updateDirectional(
        const glm::mat4& view,
        const glm::mat4& proj,
        const glm::vec3& lightDirWorld,
        float nearPlane,
        float farPlane
    );

    /**
     * Resets per-frame shadow counts and schedules.
     */
    void beginFrame();
    /**
     * Registers a spot light shadow for this frame.
     * @param desc Spot light descriptor.
     * @param invView Inverse camera view matrix.
     * @return Shadow index or -1 if unavailable.
     */
    int registerSpotShadow(const SpotShadowDesc& desc, const glm::mat4& invView);
    /**
     * Registers a point light shadow for this frame.
     * @param desc Point light descriptor.
     * @return Shadow index or -1 if unavailable.
     */
    int registerPointShadow(const PointShadowDesc& desc);

    /**
     * Renders directional cascades into the shadow map array.
     * @param meshes Meshes to draw as shadow casters.
     */
    void renderDirectionalShadows(std::initializer_list<const MeshBuffer*> meshes) const;
    /**
     * Renders spot light shadows into the shadow map array.
     * @param meshes Meshes to draw as shadow casters.
     */
    void renderSpotShadows(std::initializer_list<const MeshBuffer*> meshes) const;
    /**
     * Renders point light shadows into the cubemap array.
     * @param meshes Meshes to draw as shadow casters.
     */
    void renderPointShadows(std::initializer_list<const MeshBuffer*> meshes) const;

    /**
     * Returns the active directional cascade count.
     */
    int directionalCascadeCount() const { return dirCascadeCount_; }
    /**
     * Returns view-space-to-light-space matrices for each cascade.
     */
    const std::array<glm::mat4, kMaxCascades>& directionalMatrices() const { return dirShadowMatrices_; }
    /**
     * Returns split distances for each cascade (view-space depth).
     */
    const std::array<float, kMaxCascades>& directionalSplits() const { return dirCascadeSplits_; }
    /**
     * Returns the directional shadow map array texture id.
     */
    GLuint directionalShadowMap() const { return dirShadowMap_; }
    /**
     * Returns the directional shadow map texel size.
     */
    glm::vec2 directionalTexelSize() const { return dirTexelSize_; }
    /**
     * Returns the minimum bias for directional shadow tests.
     */
    float directionalBiasMin() const { return dirBiasMin_; }
    /**
     * Returns the slope bias factor for directional shadow tests.
     */
    float directionalBiasSlope() const { return dirBiasSlope_; }
    /**
     * Returns the PCF radius for directional shadows.
     */
    int directionalPcfRadius() const { return dirPcfRadius_; }

    /**
     * Returns the number of spot shadows registered this frame.
     */
    int spotShadowCount() const { return spotShadowCount_; }
    /**
     * Returns view-space-to-light-space matrices for spot shadows.
     */
    const std::array<glm::mat4, kMaxSpotShadows>& spotShadowMatrices() const { return spotShadowMatrices_; }
    /**
     * Returns the spot shadow map array texture id.
     */
    GLuint spotShadowMap() const { return spotShadowMap_; }
    /**
     * Returns the spot shadow map texel size.
     */
    glm::vec2 spotTexelSize() const { return spotTexelSize_; }
    /**
     * Returns the PCF radius for spot shadows.
     */
    int spotPcfRadius() const { return spotPcfRadius_; }

    /**
     * Returns the number of point shadows registered this frame.
     */
    int pointShadowCount() const { return pointShadowCount_; }
    /**
     * Returns the point shadow cubemap array texture id.
     */
    GLuint pointShadowMap() const { return pointShadowMap_; }
    /**
     * Returns the PCF disk radius for point shadows.
     */
    float pointShadowDiskRadius() const { return pointShadowDiskRadius_; }
    /**
     * Returns the PCF radius for point shadows.
     */
    int pointPcfRadius() const { return pointPcfRadius_; }

private:
    /**
     * Allocates directional shadow map resources.
     */
    void ensureDirectionalResources();
    /**
     * Allocates spot shadow map resources.
     */
    void ensureSpotResources();
    /**
     * Allocates point shadow map resources.
     */
    void ensurePointResources();
    /**
     * Releases all shadow map resources.
     */
    void destroyResources();

    ShaderProgram shadowDepthShader_;
    GLint shadowMvpLocation_{-1};

    int dirCascadeCount_{3};
    int dirShadowResolution_{2048};
    float dirSplitLambda_{0.6f};
    float dirBiasMin_{0.0015f};
    float dirBiasSlope_{0.0045f};
    int dirPcfRadius_{1};
    float dirZPadding_{10.0f};
    glm::vec2 dirTexelSize_{1.0f, 1.0f};

    int spotShadowResolution_{1024};
    int spotPcfRadius_{1};
    glm::vec2 spotTexelSize_{1.0f, 1.0f};

    int pointShadowResolution_{512};
    int pointPcfRadius_{1};
    float pointShadowDiskRadius_{0.002f};

    GLuint dirShadowMap_{0};
    GLuint dirShadowFbo_{0};
    GLuint spotShadowMap_{0};
    GLuint spotShadowFbo_{0};
    GLuint pointShadowMap_{0};
    GLuint pointShadowFbo_{0};

    std::array<glm::mat4, kMaxCascades> dirShadowViewProj_{};
    std::array<glm::mat4, kMaxCascades> dirShadowMatrices_{};
    std::array<float, kMaxCascades> dirCascadeSplits_{};

    int spotShadowCount_{0};
    std::array<glm::mat4, kMaxSpotShadows> spotShadowViewProj_{};
    std::array<glm::mat4, kMaxSpotShadows> spotShadowMatrices_{};

    int pointShadowCount_{0};
    std::array<std::array<glm::mat4, 6>, kMaxPointShadows> pointShadowViewProj_{};

    int frameIndex_{0};
    int dirUpdateEvery_{1};
    int spotUpdateEvery_{1};
    int pointUpdateEvery_{1};
};

}  // namespace render
