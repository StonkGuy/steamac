#version 450
in float gl_CullDistance[2];
layout(location = 0) out vec4 color;
void main()
{
    color = vec4(gl_CullDistance[0], gl_CullDistance[1], 0.0, 1.0);
}
