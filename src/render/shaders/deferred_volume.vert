#version 410 core
layout (location = 0) in vec3 aPos;

uniform mat4 uProj;
uniform samplerBuffer uLightBuffer;
uniform int uLightOffset;
uniform int uIsSpot;

flat out int vLightIndex;

void main() {
    int lightIndex = uLightOffset + gl_InstanceID;
    vLightIndex = lightIndex;
    int base = lightIndex * 4;

    vec4 posRadius = texelFetch(uLightBuffer, base);
    vec4 dirType = texelFetch(uLightBuffer, base + 2);
    vec4 spotParams = texelFetch(uLightBuffer, base + 3);

    vec3 lightPos = posRadius.xyz;
    float radius = posRadius.w;

    vec3 viewPos;
    if (uIsSpot == 1) {
        vec3 dir = normalize(dirType.xyz);
        float coneLength = spotParams.z;
        float coneRadius = spotParams.w * coneLength;
        vec3 scaled = vec3(aPos.x * coneRadius, aPos.y * coneRadius, aPos.z * coneLength);

        vec3 up = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
        vec3 right = normalize(cross(up, dir));
        vec3 up2 = cross(dir, right);
        vec3 rotated = right * scaled.x + up2 * scaled.y + dir * scaled.z;
        viewPos = lightPos + rotated;
    } else {
        viewPos = lightPos + aPos * radius;
    }

    gl_Position = uProj * vec4(viewPos, 1.0);
}
