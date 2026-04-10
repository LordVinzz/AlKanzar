#version 410 core
layout (location = 0) in vec3 aPos;
layout (location = 6) in uvec4 aJoints;
layout (location = 7) in vec4 aWeights;

uniform mat4 uModel;
uniform mat4 uLightViewProj;
uniform samplerBuffer uJointBuffer;
uniform int uSkinned;
uniform int uJointBaseIndex;
uniform int uJointCount;

out vec3 vWorldPos;

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
    vec4 worldPos = uModel * (resolveSkinMatrix() * vec4(aPos, 1.0));
    vWorldPos = worldPos.xyz;
    gl_Position = uLightViewProj * worldPos;
}
