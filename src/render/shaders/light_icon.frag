#version 410 core

in vec2 vTexCoord;
flat in float vOpacity;

uniform sampler2D uIconTexture;

out vec4 fragColor;

void main() {
    vec4 color = texture(uIconTexture, vec2(vTexCoord.x, 1.0 - vTexCoord.y));
    if (color.a <= 0.01) {
        discard;
    }
    fragColor = vec4(color.rgb, color.a * vOpacity);
}
