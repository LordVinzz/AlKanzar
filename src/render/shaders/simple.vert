#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec3 aColor;

uniform mat4 uMVP;
uniform vec3 uLightDir;

out vec3 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    float ndotl = max(dot(normalize(aNormal), -normalize(uLightDir)), 0.2);
    vColor = aColor * ndotl;
}
