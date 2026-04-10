#version 410 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec4 aTangent;
layout (location = 3) in vec2 aUv0;
layout (location = 4) in vec2 aUv1;
layout (location = 6) in uvec4 aJoints;
layout (location = 7) in vec4 aWeights;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform mat3 uNormalMatrix;
uniform samplerBuffer uJointBuffer;
uniform int uSkinned;
uniform int uJointBaseIndex;
uniform int uJointCount;

out vec3 vViewPos;
out vec3 vViewNormal;
out vec3 vViewTangent;
out vec3 vViewBitangent;
out vec2 vUv0;
out vec2 vUv1;

mat4 fetchJointMatrix(int jointIndex) {
    int base = (uJointBaseIndex + jointIndex) * 4;
    return mat4(
        texelFetch(uJointBuffer, base + 0),
        texelFetch(uJointBuffer, base + 1),
        texelFetch(uJointBuffer, base + 2),
        texelFetch(uJointBuffer, base + 3)
    );
}

mat4 resolveSkinMatrix() {
    if (uSkinned == 0 || uJointCount <= 0) {
        return mat4(1.0);
    }

    float totalWeight = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
    if (totalWeight <= 0.0) {
        return mat4(1.0);
    }

    mat4 skinMatrix = mat4(0.0);
    if (aWeights.x > 0.0 && int(aJoints.x) < uJointCount) {
        skinMatrix += fetchJointMatrix(int(aJoints.x)) * aWeights.x;
    }
    if (aWeights.y > 0.0 && int(aJoints.y) < uJointCount) {
        skinMatrix += fetchJointMatrix(int(aJoints.y)) * aWeights.y;
    }
    if (aWeights.z > 0.0 && int(aJoints.z) < uJointCount) {
        skinMatrix += fetchJointMatrix(int(aJoints.z)) * aWeights.z;
    }
    if (aWeights.w > 0.0 && int(aJoints.w) < uJointCount) {
        skinMatrix += fetchJointMatrix(int(aJoints.w)) * aWeights.w;
    }
    return skinMatrix;
}

void main() {
    mat4 skinMatrix = resolveSkinMatrix();
    vec4 skinnedPosition = skinMatrix * vec4(aPos, 1.0);
    vec3 skinnedNormal = mat3(skinMatrix) * aNormal;
    vec3 skinnedTangent = mat3(skinMatrix) * aTangent.xyz;

    vec3 worldNormal = normalize(uNormalMatrix * skinnedNormal);
    vec3 worldTangent = normalize(mat3(uModel) * skinnedTangent);
    worldTangent = normalize(worldTangent - worldNormal * dot(worldNormal, worldTangent));
    vec3 worldBitangent = normalize(cross(worldNormal, worldTangent)) * aTangent.w;

    vec3 viewNormal = normalize(mat3(uView) * worldNormal);
    vec3 viewTangent = normalize(mat3(uView) * worldTangent);
    vec3 viewBitangent = normalize(mat3(uView) * worldBitangent);
    vec4 viewPos = uView * uModel * skinnedPosition;

    vViewPos = viewPos.xyz;
    vViewNormal = viewNormal;
    vViewTangent = viewTangent;
    vViewBitangent = viewBitangent;
    vUv0 = aUv0;
    vUv1 = aUv1;
    gl_Position = uProj * viewPos;
}
