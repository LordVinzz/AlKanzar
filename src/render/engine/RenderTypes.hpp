#pragma once

#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace render {

enum class RenderLayer {
    Ground,
    Geometry,
    Actors,
};

enum class DebugView : int {
    Final = 0,
    Albedo = 1,
    Normal = 2,
    RoughMetal = 3,
    Depth = 4,
    Light = 5,
    ShadowMap = 6,
    ShadowFactor = 7,
    ShadowCascade = 8,
};

enum class LightType : std::uint32_t {
    Point = 0,
    Spot = 1,
};

struct MeshHandle {
    static constexpr std::size_t kInvalid = static_cast<std::size_t>(-1);

    std::size_t value{kInvalid};

    [[nodiscard]] bool valid() const {
        return value != kInvalid;
    }

    friend bool operator==(MeshHandle lhs, MeshHandle rhs) = default;
};

struct Bounds3 {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

struct CameraMatrices {
    glm::mat4 projection{1.0f};
    glm::mat4 invProjection{1.0f};
    glm::mat4 view{1.0f};
};

struct RenderFrameOptions {
    DebugView debugView{DebugView::Final};
    int shadowDebugCascade{0};
    bool showLightDebug{false};
    bool editorEnabled{false};
    bool showNavMeshOverlay{false};
    bool showNavMeshPolygonWireframe{false};
};

}  // namespace render
