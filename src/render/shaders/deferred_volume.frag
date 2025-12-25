#version 410 core
flat in int vLightIndex;
out vec4 FragColor;

uniform sampler2D uGAlbedoMetal;
uniform sampler2D uGNormalRough;
uniform sampler2D uDepth;
uniform samplerBuffer uLightBuffer;
uniform mat4 uInvProj;
uniform vec2 uScreenSize;
uniform int uIsSpot;

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

    int base = vLightIndex * 4;
    vec4 posRadius = texelFetch(uLightBuffer, base);
    vec4 colorIntensity = texelFetch(uLightBuffer, base + 1);
    vec4 dirType = texelFetch(uLightBuffer, base + 2);
    vec4 spotParams = texelFetch(uLightBuffer, base + 3);

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
    vec3 color = (diffuse + specular) * lightColor * ndotl * attenuation;

    FragColor = vec4(color, 1.0);
}
