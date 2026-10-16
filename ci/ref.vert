#version 450
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 1) out vec4 outColor;
void main() {
    gl_Position = inPos;
    outColor = inColor;
}
