#pragma once

namespace core {

enum class ComponentKind {
    Transform = 0,
    Visibility,
    Renderable,
    Material,
    LightVolume,
    PointLight,
    SpotLight,
    BoxCollider,
    SphereCollider,
    Rigidbody,
    NavAgent,
    Locomotion,
    NavSource,
    Character,
    DirectionalLight,
};

}  // namespace core
