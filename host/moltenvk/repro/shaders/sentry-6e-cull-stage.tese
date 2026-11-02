#version 450
layout(triangles, equal_spacing, ccw) in;
in gl_PerVertex { vec4 gl_Position; float gl_CullDistance[2]; } gl_in[];
out gl_PerVertex { vec4 gl_Position; float gl_CullDistance[2]; };
void main()
{
    gl_Position = gl_in[0].gl_Position;
    gl_CullDistance[0] = gl_in[0].gl_CullDistance[0];
    gl_CullDistance[1] = gl_in[0].gl_CullDistance[1];
}
