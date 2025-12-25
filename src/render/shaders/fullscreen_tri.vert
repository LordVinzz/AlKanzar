#version 410 core
out vec2 vUv;
const vec2 kVerts[3] = vec2[3](
    vec2(-1.0, -1.0),
    vec2(3.0, -1.0),
    vec2(-1.0, 3.0)
);

void main() {
    vec2 pos = kVerts[gl_VertexID];
    vUv = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
