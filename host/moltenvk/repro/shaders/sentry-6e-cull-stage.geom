#version 450
layout(points) in;
layout(points, max_vertices = 2) out;
in gl_PerVertex { vec4 gl_Position; float gl_CullDistance[2]; } gl_in[];
out gl_PerVertex { vec4 gl_Position; float gl_CullDistance[2]; };
void emit_cull(float value)
{
    gl_Position = gl_in[0].gl_Position;
    gl_CullDistance[0] = value;
    gl_CullDistance[1] = -value;
    EmitVertex();
}
void main()
{
    emit_cull(gl_in[0].gl_CullDistance[0]);
    emit_cull(1.0);
    EndPrimitive();
}
