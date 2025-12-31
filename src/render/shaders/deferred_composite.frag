#version 410 core
#define MAX_CASCADES 4
in vec2 vUv;
out vec4 FragColor;

uniform sampler2D uLightBuffer;
uniform sampler2D uGAlbedoMetal;
uniform sampler2D uGNormalRough;
uniform sampler2D uDepth;
uniform sampler2DArray uShadowMap;
uniform mat4 uShadowMatrices[MAX_CASCADES];
uniform float uCascadeSplits[MAX_CASCADES];
uniform int uCascadeCount;
uniform vec2 uShadowTexelSize;
uniform int uShadowPcfRadius;
uniform mat4 uInvProj;
uniform vec3 uDirLightDir;
uniform float uShadowBiasMin;
uniform float uShadowBiasSlope;
uniform int uShadowDebugCascade;
uniform int uDebugMode;

vec3 tonemap(vec3 color) {
    return color / (color + vec3(1.0));
}

vec3 reconstructViewPos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = uInvProj * ndc;
    return view.xyz / view.w;
}

float sampleShadowMap(vec3 shadowCoord, int layer, float bias) {
    if (shadowCoord.z > 1.0 || shadowCoord.x < 0.0 || shadowCoord.x > 1.0 || shadowCoord.y < 0.0 || shadowCoord.y > 1.0) {
        return 1.0;
    }
    float shadow = 0.0;
    int taps = 0;
    for (int y = -uShadowPcfRadius; y <= uShadowPcfRadius; ++y) {
        for (int x = -uShadowPcfRadius; x <= uShadowPcfRadius; ++x) {
            vec2 offset = vec2(x, y) * uShadowTexelSize;
            float depth = texture(uShadowMap, vec3(shadowCoord.xy + offset, layer)).r;
            shadow += (shadowCoord.z - bias <= depth) ? 1.0 : 0.0;
            taps++;
        }
    }
    return shadow / max(float(taps), 1.0);
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
    } else if (uDebugMode == 6) {
        int cascade = clamp(uShadowDebugCascade, 0, uCascadeCount - 1);
        float depth = texture(uShadowMap, vec3(vUv, cascade)).r;
        color = vec3(depth);
    } else if (uDebugMode == 7 || uDebugMode == 8) {
        float depth = texture(uDepth, vUv).r;
        if (depth >= 0.99999) {
            color = vec3(0.0);
        } else {
            vec4 normalRough = texture(uGNormalRough, vUv);
            vec3 normal = normalize(normalRough.xyz);
            vec3 viewPos = reconstructViewPos(vUv, depth);
            float viewDepth = -viewPos.z;
            int cascade = 0;
            for (int i = 0; i < uCascadeCount - 1; ++i) {
                if (viewDepth > uCascadeSplits[i]) {
                    cascade = i + 1;
                }
            }

            if (uDebugMode == 8) {
                if (cascade == 0) {
                    color = vec3(0.85, 0.15, 0.15);
                } else if (cascade == 1) {
                    color = vec3(0.15, 0.85, 0.15);
                } else if (cascade == 2) {
                    color = vec3(0.15, 0.25, 0.85);
                } else {
                    color = vec3(0.85, 0.85, 0.15);
                }
            } else {
                vec3 L = normalize(-uDirLightDir);
                float ndotl = max(dot(normal, L), 0.0);
                float bias = max(uShadowBiasMin, uShadowBiasSlope * (1.0 - ndotl));
                vec4 shadowPos = uShadowMatrices[cascade] * vec4(viewPos, 1.0);
                vec3 shadowCoord = shadowPos.xyz / shadowPos.w;
                shadowCoord = shadowCoord * 0.5 + 0.5;
                float shadow = sampleShadowMap(shadowCoord, cascade, bias);
                color = vec3(shadow);
            }
        }
    } else {
        color = texture(uLightBuffer, vUv).rgb;
    }

    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
    FragColor = vec4(color, 1.0);
}
