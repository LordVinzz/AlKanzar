#version 410 core
flat in int vLightIndex;
out vec4 FragColor;

uniform sampler2D uGAlbedoMetal;
uniform sampler2D uGNormalRough;
uniform sampler2D uGEmissiveAo;
uniform sampler2D uGClearcoat;
uniform sampler2D uDepth;
uniform samplerBuffer uLightBuffer;
uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform vec2 uScreenSize;
uniform int uIsSpot;
uniform vec3 uVolumeMin;
uniform vec3 uVolumeMax;
uniform sampler2DArray uSpotShadowMap;
uniform samplerCubeArray uPointShadowMap;
uniform mat4 uSpotShadowMatrices[4];
uniform vec3 uSpotShadowPositions[4];
uniform float uSpotShadowFarPlanes[4];
uniform int uSpotShadowCount;
uniform vec2 uSpotShadowTexelSize;
uniform int uSpotShadowPcfRadius;
uniform int uPointShadowCount;
uniform float uPointShadowDiskRadius;
uniform int uPointShadowPcfRadius;

float sampleShadowMap2D(sampler2DArray map, vec3 uvw, int layer, float bias, vec2 texelSize, int radius) {
    if (uvw.z < 0.0 || uvw.z > 1.0 || uvw.x < 0.0 || uvw.x > 1.0 || uvw.y < 0.0 || uvw.y > 1.0) {
        return 1.0;
    }
    float shadow = 0.0;
    int taps = 0;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            vec2 offset = vec2(x, y) * texelSize;
            float depth = texture(map, vec3(uvw.xy + offset, layer)).r;
            shadow += (uvw.z - bias <= depth) ? 1.0 : 0.0;
            taps++;
        }
    }
    return shadow / max(float(taps), 1.0);
}

float sampleShadowMapCube(
    samplerCubeArray map,
    vec3 dir,
    float depth,
    int layer,
    float bias,
    float diskRadius,
    int radius
) {
    vec3 up = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(up, dir));
    vec3 upDir = cross(dir, right);

    float shadow = 0.0;
    int taps = 0;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            vec2 offset = vec2(x, y) * diskRadius;
            vec3 sampleDir = normalize(dir + right * offset.x + upDir * offset.y);
            float mapDepth = texture(map, vec4(sampleDir, layer)).r;
            shadow += (depth - bias <= mapDepth) ? 1.0 : 0.0;
            taps++;
        }
    }
    return shadow / max(float(taps), 1.0);
}

vec3 reconstructViewPos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = uInvProj * ndc;
    return view.xyz / view.w;
}

void main() {
    vec2 uv = gl_FragCoord.xy / uScreenSize;
    float depth = texture(uDepth, uv).r;
    if (depth >= 0.99999) {
        discard;
    }

    vec4 albedoMetal = texture(uGAlbedoMetal, uv);
    vec4 normalRough = texture(uGNormalRough, uv);
    vec4 emissiveAo = texture(uGEmissiveAo, uv);
    vec2 clearcoatSample = texture(uGClearcoat, uv).rg;

    vec3 albedo = albedoMetal.rgb;
    float metallic = albedoMetal.a;
    vec3 normal = normalize(normalRough.xyz);
    float roughness = normalRough.a;
    float ao = emissiveAo.a;
    float clearcoat = clearcoatSample.x;
    float clearcoatRoughness = clearcoatSample.y;

    vec3 viewPos = reconstructViewPos(uv, depth);
    vec3 worldPos = vec3(uInvView * vec4(viewPos, 1.0));
    vec3 normalWorld = normalize(mat3(uInvView) * normal);
    bvec3 belowMin = lessThan(worldPos, uVolumeMin);
    bvec3 aboveMax = greaterThan(worldPos, uVolumeMax);
    if (any(belowMin) || any(aboveMax)) {
        discard;
    }

    int base = vLightIndex * 5;
    vec4 posRadius = texelFetch(uLightBuffer, base);
    vec4 colorIntensity = texelFetch(uLightBuffer, base + 1);
    vec4 dirType = texelFetch(uLightBuffer, base + 2);
    vec4 spotParams = texelFetch(uLightBuffer, base + 3);
    vec4 shadowInfo = texelFetch(uLightBuffer, base + 4);

    vec3 lightPos = posRadius.xyz;
    float radius = posRadius.w;
    vec3 toLight = lightPos - viewPos;
    float dist2 = dot(toLight, toLight);
    if (dist2 > radius * radius) {
        discard;
    }

    float dist = sqrt(dist2);
    vec3 L = toLight / max(dist, 0.0001);
    float attenuation = clamp(1.0 - dist / radius, 0.0, 1.0);
    attenuation *= attenuation;
    if (uIsSpot == 0) {
        float d2r2 = dist2 / max(radius * radius, 0.0001);
        float windowing = clamp(1.0 - d2r2 * d2r2, 0.0, 1.0);
        attenuation = (windowing * windowing) / (dist2 + 1.0);
    }

    if (uIsSpot == 1) {
        vec3 spotDir = normalize(dirType.xyz);
        float cosTheta = dot(normalize(-L), spotDir);
        float inner = spotParams.x;
        float outer = spotParams.y;
        float spot = smoothstep(outer, inner, cosTheta);
        attenuation *= spot;
    }

    float ndotl = max(dot(normal, L), 0.0);
    if (ndotl <= 0.0) {
        discard;
    }

    vec3 V = normalize(-viewPos);
    vec3 H = normalize(L + V);
    float specPower = mix(96.0, 6.0, roughness);
    float spec = pow(max(dot(normal, H), 0.0), specPower);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 diffuse = (1.0 - metallic) * albedo / 3.14159265;
    vec3 specular = F0 * spec;
    float clearcoatSpecPower = mix(144.0, 12.0, clearcoatRoughness);
    vec3 clearcoatSpecular = vec3(0.04 * clearcoat * pow(max(dot(normal, H), 0.0), clearcoatSpecPower));

    vec3 lightColor = colorIntensity.rgb * colorIntensity.w;
    float shadow = 1.0;
    int shadowType = int(shadowInfo.x + 0.5);
    int shadowIndex = int(shadowInfo.y + 0.5);
    float bias = max(shadowInfo.z, shadowInfo.w * (1.0 - ndotl));

    if (shadowType == 1 && shadowIndex >= 0 && shadowIndex < uSpotShadowCount) {
        vec3 lightWorld = uSpotShadowPositions[shadowIndex];
        float farPlane = max(uSpotShadowFarPlanes[shadowIndex], 0.0001);
        float worldDist = length(worldPos - lightWorld);
        float receiverOffset = max(
            bias * radius,
            worldDist * max(uSpotShadowTexelSize.x, uSpotShadowTexelSize.y) * float(max(uSpotShadowPcfRadius, 1) + 1)
        );
        vec3 sampleViewPos = viewPos + normal * receiverOffset;
        vec3 sampleWorldPos = worldPos + normalWorld * receiverOffset;
        vec4 shadowPos = uSpotShadowMatrices[shadowIndex] * vec4(sampleViewPos, 1.0);
        vec3 shadowCoord = shadowPos.xyz / shadowPos.w;
        shadowCoord = shadowCoord * 0.5 + 0.5;
        shadowCoord.z = clamp(length(sampleWorldPos - lightWorld) / farPlane, 0.0, 1.0);
        shadow = sampleShadowMap2D(
            uSpotShadowMap,
            shadowCoord,
            shadowIndex,
            0.0,
            uSpotShadowTexelSize,
            uSpotShadowPcfRadius
        );
    } else if (shadowType == 2 && shadowIndex >= 0 && shadowIndex < uPointShadowCount) {
        vec3 lightWorld = vec3(uInvView * vec4(lightPos, 1.0));
        vec3 toLightWorld = worldPos - lightWorld;
        float worldDist = length(toLightWorld);
        float receiverOffsetWorld = max(
            bias * radius,
            worldDist * uPointShadowDiskRadius * float(max(uPointShadowPcfRadius, 1) + 1)
        );
        vec3 sampleWorldPos = worldPos + normalWorld * receiverOffsetWorld;
        vec3 sampleToLightWorld = sampleWorldPos - lightWorld;
        float depth01 = clamp(length(sampleToLightWorld) / radius, 0.0, 1.0);
        vec3 dir = normalize(sampleToLightWorld);
        shadow = sampleShadowMapCube(
            uPointShadowMap,
            dir,
            depth01,
            shadowIndex,
            0.0,
            uPointShadowDiskRadius,
            uPointShadowPcfRadius
        );
    }

    float visibility = mix(1.0, ao, 0.35);
    vec3 color = (diffuse + specular + clearcoatSpecular) * lightColor * ndotl * attenuation * shadow * visibility;
    FragColor = vec4(color, 1.0);
}
