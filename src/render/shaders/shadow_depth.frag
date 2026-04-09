#version 410 core

in vec3 vWorldPos;

uniform int uShadowMode;
uniform vec3 uLightPositionWorld;
uniform float uLightFarPlane;

void main() {
    if (uShadowMode == 1) {
        float lightDistance = length(vWorldPos - uLightPositionWorld);
        gl_FragDepth = clamp(lightDistance / max(uLightFarPlane, 0.0001), 0.0, 1.0);
    } else {
        gl_FragDepth = gl_FragCoord.z;
    }
}
