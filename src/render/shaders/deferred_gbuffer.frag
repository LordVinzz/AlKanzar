#version 410 core
in vec3 vViewPos;
in vec3 vViewNormal;
in vec3 vViewTangent;
in vec3 vViewBitangent;
in vec2 vUv0;
in vec2 vUv1;

layout(location = 0) out vec4 gAlbedoMetal;
layout(location = 1) out vec4 gNormalRough;
layout(location = 2) out vec4 gEmissiveAo;
layout(location = 3) out vec4 gClearcoat;
layout(location = 4) out float gDepth;

uniform sampler2D uBaseColorTexture;
uniform sampler2D uMetallicRoughnessTexture;
uniform sampler2D uNormalTexture;
uniform sampler2D uAoTexture;
uniform sampler2D uEmissiveTexture;
uniform sampler2D uAlphaTexture;
uniform sampler2D uClearcoatTexture;
uniform sampler2D uDetailNormalTexture;
uniform sampler2D uHeightTexture;

uniform vec3 uBaseColorFactor;
uniform float uMetallicFactor;
uniform float uRoughnessFactor;
uniform float uNormalScale;
uniform float uAoStrength;
uniform vec3 uEmissiveFactor;
uniform float uEmissiveStrength;
uniform float uAlphaFactor;
uniform int uAlphaMode;
uniform float uAlphaCutoff;
uniform float uClearcoatFactor;
uniform float uClearcoatRoughness;
uniform float uDetailNormalScale;
uniform float uHeightScale;

uniform int uBaseColorUvSet;
uniform int uMetallicRoughnessUvSet;
uniform int uNormalUvSet;
uniform int uAoUvSet;
uniform int uEmissiveUvSet;
uniform int uAlphaUvSet;
uniform int uClearcoatUvSet;
uniform int uDetailNormalUvSet;
uniform int uHeightUvSet;

uniform mat3 uBaseColorUvTransform;
uniform mat3 uMetallicRoughnessUvTransform;
uniform mat3 uNormalUvTransform;
uniform mat3 uAoUvTransform;
uniform mat3 uEmissiveUvTransform;
uniform mat3 uAlphaUvTransform;
uniform mat3 uClearcoatUvTransform;
uniform mat3 uDetailNormalUvTransform;
uniform mat3 uHeightUvTransform;

vec2 selectUv(int uvSet) {
    return uvSet == 1 ? vUv1 : vUv0;
}

vec2 transformUv(vec2 uv, mat3 transformMatrix) {
    return (transformMatrix * vec3(uv, 1.0)).xy;
}

void main() {
    mat3 tbn = mat3(normalize(vViewTangent), normalize(vViewBitangent), normalize(vViewNormal));
    vec3 viewDirTs = normalize(transpose(tbn) * normalize(-vViewPos));

    vec2 heightUv = transformUv(selectUv(uHeightUvSet), uHeightUvTransform);
    float height = texture(uHeightTexture, heightUv).r;
    vec2 parallaxOffset = (height - 0.5) * uHeightScale * viewDirTs.xy;

    vec2 baseUv = transformUv(selectUv(uBaseColorUvSet), uBaseColorUvTransform) - parallaxOffset;
    vec2 ormUv = transformUv(selectUv(uMetallicRoughnessUvSet), uMetallicRoughnessUvTransform) - parallaxOffset;
    vec2 normalUv = transformUv(selectUv(uNormalUvSet), uNormalUvTransform) - parallaxOffset;
    vec2 aoUv = transformUv(selectUv(uAoUvSet), uAoUvTransform) - parallaxOffset;
    vec2 emissiveUv = transformUv(selectUv(uEmissiveUvSet), uEmissiveUvTransform) - parallaxOffset;
    vec2 alphaUv = transformUv(selectUv(uAlphaUvSet), uAlphaUvTransform) - parallaxOffset;
    vec2 clearcoatUv = transformUv(selectUv(uClearcoatUvSet), uClearcoatUvTransform) - parallaxOffset;
    vec2 detailUv = transformUv(selectUv(uDetailNormalUvSet), uDetailNormalUvTransform) - parallaxOffset;

    vec4 baseSample = texture(uBaseColorTexture, baseUv);
    vec3 albedo = uBaseColorFactor * baseSample.rgb;
    float alpha = baseSample.a * texture(uAlphaTexture, alphaUv).r * uAlphaFactor;
    if (uAlphaMode == 1 && alpha < uAlphaCutoff) {
        discard;
    }

    vec3 normalTs = texture(uNormalTexture, normalUv).xyz * 2.0 - 1.0;
    normalTs.xy *= uNormalScale;
    vec3 detailNormalTs = texture(uDetailNormalTexture, detailUv).xyz * 2.0 - 1.0;
    detailNormalTs.xy *= uDetailNormalScale;
    normalTs.xy += detailNormalTs.xy;
    normalTs = normalize(normalTs);

    vec3 normal = normalize(tbn * normalTs);
    vec3 orm = texture(uMetallicRoughnessTexture, ormUv).rgb;
    float metallic = clamp(uMetallicFactor * orm.b, 0.0, 1.0);
    float roughness = clamp(uRoughnessFactor * orm.g, 0.05, 1.0);
    float ao = mix(1.0, orm.r * texture(uAoTexture, aoUv).r, clamp(uAoStrength, 0.0, 1.0));
    vec3 emissive = uEmissiveFactor * texture(uEmissiveTexture, emissiveUv).rgb * uEmissiveStrength;
    vec2 clearcoatSample = texture(uClearcoatTexture, clearcoatUv).rg;
    float clearcoat = clamp(uClearcoatFactor * clearcoatSample.r, 0.0, 1.0);
    float clearcoatRoughness = clamp(uClearcoatRoughness * max(clearcoatSample.g, 0.001), 0.04, 1.0);

    gAlbedoMetal = vec4(albedo, metallic);
    gNormalRough = vec4(normal, roughness);
    gEmissiveAo = vec4(emissive, ao);
    gClearcoat = vec4(clearcoat, clearcoatRoughness, alpha, 0.0);
    gDepth = gl_FragCoord.z;
}
