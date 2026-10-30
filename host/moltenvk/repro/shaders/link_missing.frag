#version 450
layout(location = 3) in vec4 missingVarying;
layout(location = 0) out vec4 color;
void main() { color = missingVarying; }
