#version 450
layout(vertices = 3) out;
in gl_PerVertex { vec4 gl_Position; float gl_CullDistance[2]; } gl_in[];
out gl_PerVertex { vec4 gl_Position; float gl_CullDistance[2]; } gl_out[];
void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    gl_out[gl_InvocationID].gl_CullDistance[0] = gl_in[gl_InvocationID].gl_CullDistance[0];
    gl_out[gl_InvocationID].gl_CullDistance[1] = gl_in[gl_InvocationID].gl_CullDistance[1];
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
}
