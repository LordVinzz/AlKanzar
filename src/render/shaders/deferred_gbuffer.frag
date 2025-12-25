#version 410 core
in vec3 vNormal;
in vec3 vAlbedo;

layout(location = 0) out vec4 gAlbedoMetal;
layout(location = 1) out vec4 gNormalRough;
layout(location = 2) out float gDepth;

uniform float uMetallic;
uniform float uRoughness;

void main() {
    vec3 normal = normalize(vNormal);
    gAlbedoMetal = vec4(vAlbedo, uMetallic);
    gNormalRough = vec4(normal, uRoughness);
    gDepth = gl_FragCoord.z;
}
