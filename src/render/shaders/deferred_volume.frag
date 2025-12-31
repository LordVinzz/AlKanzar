#version 410 core
flat in int vLightIndex;
out vec4 FragColor;

uniform sampler2D uGAlbedoMetal;
uniform sampler2D uGNormalRough;
uniform sampler2D uDepth;
uniform samplerBuffer uLightBuffer;
uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform vec2 uScreenSize;
uniform int uIsSpot;
uniform sampler2DArray uSpotShadowMap;
uniform samplerCubeArray uPointShadowMap;
uniform mat4 uSpotShadowMatrices[4];
uniform int uSpotShadowCount;
uniform vec2 uSpotShadowTexelSize;
uniform int uSpotShadowPcfRadius;
uniform int uPointShadowCount;
uniform float uPointShadowDiskRadius;
uniform int uPointShadowPcfRadius;

float sampleShadowMap2D(sampler2DArray map, vec3 uvw, int layer, float bias, vec2 texelSize, int radius) {
    if (uvw.z > 1.0 || uvw.x < 0.0 || uvw.x > 1.0 || uvw.y < 0.0 || uvw.y > 1.0) {
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

    vec3 albedo = albedoMetal.rgb;
    float metallic = albedoMetal.a;
    vec3 normal = normalize(normalRough.xyz);
    float roughness = normalRough.a;

    vec3 viewPos = reconstructViewPos(uv, depth);

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
    float specPower = mix(64.0, 4.0, roughness);
    float spec = pow(max(dot(normal, H), 0.0), specPower);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 diffuse = (1.0 - metallic) * albedo / 3.14159265;
    vec3 specular = F0 * spec;

    vec3 lightColor = colorIntensity.rgb * colorIntensity.w;
    float shadow = 1.0;
    int shadowType = int(shadowInfo.x + 0.5);
    int shadowIndex = int(shadowInfo.y + 0.5);
    float bias = max(shadowInfo.z, shadowInfo.w * (1.0 - ndotl));

    if (shadowType == 1 && shadowIndex >= 0 && shadowIndex < uSpotShadowCount) {
        vec4 shadowPos = uSpotShadowMatrices[shadowIndex] * vec4(viewPos, 1.0);
        vec3 shadowCoord = shadowPos.xyz / shadowPos.w;
        shadowCoord = shadowCoord * 0.5 + 0.5;
        shadow = sampleShadowMap2D(
            uSpotShadowMap,
            shadowCoord,
            shadowIndex,
            bias,
            uSpotShadowTexelSize,
            uSpotShadowPcfRadius
        );
    } else if (shadowType == 2 && shadowIndex >= 0 && shadowIndex < uPointShadowCount) {
        vec3 worldPos = vec3(uInvView * vec4(viewPos, 1.0));
        vec3 lightWorld = vec3(uInvView * vec4(lightPos, 1.0));
        vec3 toLightWorld = worldPos - lightWorld;
        float worldDist = length(toLightWorld);
        float depth01 = clamp(worldDist / radius, 0.0, 1.0);
        vec3 dir = normalize(toLightWorld);
        shadow = sampleShadowMapCube(
            uPointShadowMap,
            dir,
            depth01,
            shadowIndex,
            bias,
            uPointShadowDiskRadius,
            uPointShadowPcfRadius
        );
    }

    vec3 color = (diffuse + specular) * lightColor * ndotl * attenuation * shadow;

    FragColor = vec4(color, 1.0);
}
