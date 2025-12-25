#version 410 core
in vec2 vUv;
out vec4 FragColor;

uniform sampler2D uLightBuffer;
uniform sampler2D uGAlbedoMetal;
uniform sampler2D uGNormalRough;
uniform sampler2D uDepth;
uniform int uDebugMode;

vec3 tonemap(vec3 color) {
    return color / (color + vec3(1.0));
}

void main() {
    vec3 color;
    if (uDebugMode == 0) {
        vec3 hdr = texture(uLightBuffer, vUv).rgb;
        color = tonemap(hdr);
    } else if (uDebugMode == 1) {
        color = texture(uGAlbedoMetal, vUv).rgb;
    } else if (uDebugMode == 2) {
        vec3 normal = normalize(texture(uGNormalRough, vUv).xyz);
        color = normal * 0.5 + 0.5;
    } else if (uDebugMode == 3) {
        float rough = texture(uGNormalRough, vUv).a;
        float metal = texture(uGAlbedoMetal, vUv).a;
        color = vec3(rough, metal, 0.0);
    } else if (uDebugMode == 4) {
        float depth = texture(uDepth, vUv).r;
        color = vec3(depth);
    } else {
        color = texture(uLightBuffer, vUv).rgb;
    }

    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
    FragColor = vec4(color, 1.0);
}
