#version 410 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec4 aTangent;
layout (location = 3) in vec2 aUv0;
layout (location = 4) in vec2 aUv1;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform mat3 uNormalMatrix;

out vec3 vViewPos;
out vec3 vViewNormal;
out vec3 vViewTangent;
out vec3 vViewBitangent;
out vec2 vUv0;
out vec2 vUv1;

void main() {
    vec3 worldNormal = normalize(uNormalMatrix * aNormal);
    vec3 worldTangent = normalize(mat3(uModel) * aTangent.xyz);
    worldTangent = normalize(worldTangent - worldNormal * dot(worldNormal, worldTangent));
    vec3 worldBitangent = normalize(cross(worldNormal, worldTangent)) * aTangent.w;

    vec4 viewPos = uView * uModel * vec4(aPos, 1.0);
    vViewPos = viewPos.xyz;
    vViewNormal = normalize(mat3(uView) * worldNormal);
    vViewTangent = normalize(mat3(uView) * worldTangent);
    vViewBitangent = normalize(mat3(uView) * worldBitangent);
    vUv0 = aUv0;
    vUv1 = aUv1;
    gl_Position = uProj * viewPos;
}
