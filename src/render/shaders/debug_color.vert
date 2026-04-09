#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 2) in vec3 aColor;

uniform mat4 uMVP;
uniform vec4 uColor;

out vec4 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = vec4(aColor, 1.0) * uColor;
}
