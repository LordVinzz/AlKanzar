#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 3) in vec2 aTexCoord;
layout(location = 8) in vec4 aClipCenter;
layout(location = 9) in float aOpacity;

uniform vec2 uSizeNdc;

out vec2 vTexCoord;
flat out float vOpacity;

void main() {
    vec2 clipOffset = aPosition.xy * uSizeNdc * aClipCenter.w;
    gl_Position = aClipCenter + vec4(clipOffset, 0.0, 0.0);
    vTexCoord = aTexCoord;
    vOpacity = aOpacity;
}
