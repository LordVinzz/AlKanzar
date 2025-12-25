#version 410 core
in vec2 vUv;
out vec4 FragColor;

uniform sampler2D uGAlbedoMetal;
uniform sampler2D uGNormalRough;
uniform sampler2D uDepth;
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

    vec3 color = uAmbient * albedo;
    color += (diffuse + specular) * uDirLightColor * uDirLightIntensity * ndotl;

    FragColor = vec4(color, 1.0);
}
