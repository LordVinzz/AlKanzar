#version 410 core
#define MAX_CASCADES 4
in vec2 vUv;
out vec4 FragColor;

uniform sampler2D uGAlbedoMetal;
uniform sampler2D uGNormalRough;
uniform sampler2D uDepth;
uniform sampler2DArray uShadowMap;
uniform mat4 uShadowMatrices[MAX_CASCADES];
uniform float uCascadeSplits[MAX_CASCADES];
uniform int uCascadeCount;
uniform vec2 uShadowTexelSize;
uniform float uShadowBiasMin;
uniform float uShadowBiasSlope;
uniform int uShadowPcfRadius;
uniform mat4 uInvProj;
uniform vec3 uDirLightDir;
uniform vec3 uDirLightColor;
uniform float uDirLightIntensity;
uniform vec3 uAmbient;

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
    float depth = texture(uDepth, vUv).r;
    if (depth >= 0.99999) {
        FragColor = vec4(0.0);
        return;
    }

    vec4 albedoMetal = texture(uGAlbedoMetal, vUv);
    vec4 normalRough = texture(uGNormalRough, vUv);

    vec3 albedo = albedoMetal.rgb;
    float metallic = albedoMetal.a;
    vec3 normal = normalize(normalRough.xyz);
    float roughness = normalRough.a;

    vec3 viewPos = reconstructViewPos(vUv, depth);
    vec3 V = normalize(-viewPos);
    vec3 L = normalize(-uDirLightDir);
    vec3 H = normalize(L + V);

    float ndotl = max(dot(normal, L), 0.0);
    float specPower = mix(64.0, 4.0, roughness);
    float spec = pow(max(dot(normal, H), 0.0), specPower);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 diffuse = (1.0 - metallic) * albedo / 3.14159265;
    vec3 specular = F0 * spec;

    float viewDepth = -viewPos.z;
    int cascade = 0;
    for (int i = 0; i < uCascadeCount - 1; ++i) {
        if (viewDepth > uCascadeSplits[i]) {
            cascade = i + 1;
        }
    }

    vec4 shadowPos = uShadowMatrices[cascade] * vec4(viewPos, 1.0);
    vec3 shadowCoord = shadowPos.xyz / shadowPos.w;
    shadowCoord = shadowCoord * 0.5 + 0.5;
    float bias = max(uShadowBiasMin, uShadowBiasSlope * (1.0 - ndotl));
    float shadow = sampleShadowMap(shadowCoord, cascade, bias);

    vec3 color = uAmbient * albedo;
    color += (diffuse + specular) * uDirLightColor * uDirLightIntensity * ndotl * shadow;

    FragColor = vec4(color, 1.0);
}
