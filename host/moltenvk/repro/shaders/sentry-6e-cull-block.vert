#version 450
out gl_PerVertex {
    vec4 gl_Position;
    float gl_CullDistance[2];
};
void update_cull()
{
    gl_CullDistance[1] = gl_CullDistance[0] - 1.0;
}
void main()
{
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    gl_CullDistance[0] = 1.0;
    gl_CullDistance[1] = 0.0;
    update_cull();
}
