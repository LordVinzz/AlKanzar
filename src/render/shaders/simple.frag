#version 410 core
in vec3 vViewPos;
in vec3 vViewNormal;
in vec3 vViewTangent;
in vec3 vViewBitangent;
in vec2 vUv0;
in vec2 vUv1;

out vec4 FragColor;

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
uniform vec3 uLightDir;

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

vec3 tonemap(vec3 color) {
    return color / (color + vec3(1.0));
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
    vec3 baseColor = uBaseColorFactor * baseSample.rgb;
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
    float ao = mix(1.0, orm.r * texture(uAoTexture, aoUv).r, clamp(uAoStrength, 0.0, 1.0));
    float roughness = clamp(uRoughnessFactor * orm.g, 0.05, 1.0);
    float metallic = clamp(uMetallicFactor * orm.b, 0.0, 1.0);

    vec3 emissive = uEmissiveFactor * texture(uEmissiveTexture, emissiveUv).rgb * uEmissiveStrength;
    vec2 clearcoatSample = texture(uClearcoatTexture, clearcoatUv).rg;
    float clearcoat = clamp(uClearcoatFactor * clearcoatSample.r, 0.0, 1.0);
    float clearcoatRoughness = clamp(uClearcoatRoughness * max(clearcoatSample.g, 0.001), 0.04, 1.0);

    vec3 L = normalize(-uLightDir);
    vec3 V = normalize(-vViewPos);
    vec3 H = normalize(L + V);
    float ndotl = max(dot(normal, L), 0.0);
    float ndoth = max(dot(normal, H), 0.0);

    vec3 F0 = mix(vec3(0.04), baseColor, metallic);
    vec3 diffuse = (1.0 - metallic) * baseColor / 3.14159265;
    float specPower = mix(96.0, 6.0, roughness);
    vec3 specular = F0 * pow(ndoth, specPower);
    float clearcoatSpecPower = mix(144.0, 12.0, clearcoatRoughness);
    vec3 clearcoatSpecular = vec3(0.04 * clearcoat * pow(ndoth, clearcoatSpecPower));

    vec3 lightColor = vec3(0.85);
    vec3 color = (diffuse + specular + clearcoatSpecular) * lightColor * ndotl;
    color *= mix(1.0, ao, 0.35);
    color += baseColor * ao * 0.08;
    color += emissive;
    color = pow(clamp(tonemap(color), 0.0, 1.0), vec3(1.0 / 2.2));

    FragColor = vec4(color, uAlphaMode == 2 ? alpha : 1.0);
}
