#version 410 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec3 aColor;

uniform mat4 uMVP;
uniform mat4 uView;

out vec3 vNormal;
out vec3 vAlbedo;

void main() {
    vNormal = mat3(uView) * aNormal;
    vAlbedo = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
