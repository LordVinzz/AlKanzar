#version 410 core
layout (location = 0) in vec3 aPos;

uniform mat4 uLightMVP;

out vec3 vWorldPos;

void main() {
    vWorldPos = aPos;
    gl_Position = uLightMVP * vec4(aPos, 1.0);
}
