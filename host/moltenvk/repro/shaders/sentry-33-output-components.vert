#version 450
layout(location = 0, component = 0) out vec3 POSITION;
layout(location = 0, component = 3) out float EXTRA;
layout(location = 1, component = 0) out vec2 TEXCOORD;
layout(location = 1, component = 2) out vec2 OTHER;
void update_outputs()
{
    POSITION.z = POSITION.x + POSITION.y;
    TEXCOORD.y = TEXCOORD.x;
}
void main()
{
    POSITION.x = 1.0;
    POSITION.y = 2.0;
    POSITION.z = 3.0;
    EXTRA = 1.0;
    TEXCOORD.x = 4.0;
    TEXCOORD.y = 5.0;
    OTHER = vec2(6.0);
    update_outputs();
    gl_Position = vec4(POSITION, 1.0);
}
