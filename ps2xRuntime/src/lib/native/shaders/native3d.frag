#version 450
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vST;
layout(location = 0) out vec4 FragColor;

layout(push_constant, std430) uniform Constants
{
    mat4 objectToClip;
    vec4 screen;
    vec4 target;
    vec4 misc;
} pc;

void main()
{
    vec3 tint = unpackUnorm4x8(floatBitsToUint(pc.misc.y)).rgb;
    FragColor = vec4(mix(vColor.rgb, tint, pc.misc.z), pc.misc.w);
}
