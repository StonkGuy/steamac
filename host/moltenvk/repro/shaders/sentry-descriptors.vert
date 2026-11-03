#version 450
layout(set = 1, binding = 1) uniform Params { vec4 position; } params;
void main() { gl_Position = params.position; }
