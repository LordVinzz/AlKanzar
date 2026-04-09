#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 3) in vec2 aTexCoord;

uniform vec4 uClipCenter;
uniform vec2 uSizeNdc;

out vec2 vTexCoord;

void main() {
    vec2 clipOffset = aPosition.xy * uSizeNdc * uClipCenter.w;
    gl_Position = uClipCenter + vec4(clipOffset, 0.0, 0.0);
    vTexCoord = aTexCoord;
}
