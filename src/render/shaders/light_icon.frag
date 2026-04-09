#version 410 core

in vec2 vTexCoord;

uniform sampler2D uIconTexture;
uniform float uOpacity;

out vec4 fragColor;

void main() {
    vec4 color = texture(uIconTexture, vec2(vTexCoord.x, 1.0 - vTexCoord.y));
    if (color.a <= 0.01) {
        discard;
    }
    fragColor = vec4(color.rgb, color.a * uOpacity);
}
