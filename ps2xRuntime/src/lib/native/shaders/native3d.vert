#version 450
// Native 3D: object space -> GS window coordinates exactly as the VU1 microprograms compute them.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor; // GS colour bytes, 0x80 = 1.0
layout(location = 2) in vec2 aST;

layout(push_constant, std430) uniform Constants
{
    mat4 objectToClip;
    vec4 screen; // xy = scale, zw = offset (GS 12.4 window units)
    vec4 target; // xy = XYOFFSET (12.4), zw = 1 / target size in pixels
    vec4 misc;   // x = depth numerator (normalised), y = packed debug tint, z = tint mix, w = output alpha
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vST;

void main()
{
    vec4 clip = pc.objectToClip * vec4(aPos, 1.0);
    float w = clip.w;
    vec2 window = clip.xy / w * pc.screen.xy + pc.screen.zw;
    vec2 pixel = (window - pc.target.xy) * (1.0 / 16.0);
    vec2 ndc = pixel * pc.target.zw * 2.0 - 1.0;
    // GS Z is depth / w (larger is nearer); z_ndc = misc.x / w reproduces it, tested with GREATER_OR_EQUAL.
    gl_Position = vec4(ndc * w, pc.misc.x, w);
    vColor = min(aColor * (255.0 / 128.0), vec4(1.0));
    vST = aST;
}
