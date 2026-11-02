#version 450
layout(location=0) in vec4 a[12];
layout(set=0,binding=17,std430) readonly buffer Data { float values[]; } data;
void main() { vec4 p=vec4(0); for(int i=0;i<12;i++) p+=a[i]; gl_Position=p+vec4(float(data.values.length())); }
