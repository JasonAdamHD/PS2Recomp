#version 450
// GS texture function (TFX/TCC) and alpha test on the GS's byte scale (0x80 = 1.0).
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vSTQ;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant, std430) uniform Constants
{
    mat4 objectToClip;
    vec4 screen;
    vec4 target;
    vec4 misc;
    vec4 shade; // x = AREF, y = bits: 0 TME, 1 TCC, 2-3 TFX, 4 ATE, 5-7 ATST, 8-9 AFAIL, 10 FBA, 11 screen-space
} pc;

bool alphaPasses(float a, uint atst, float aref)
{
    switch (atst)
    {
    case 0u: return false;
    case 1u: return true;
    case 2u: return a < aref;
    case 3u: return a <= aref;
    case 4u: return a == aref;
    case 5u: return a >= aref;
    case 6u: return a > aref;
    default: return a != aref;
    }
}

void main()
{
    uint bits = floatBitsToUint(pc.shade.y);
    vec4 v = floor(vColor * 255.0 + 0.5);
    vec4 c = v;
    if ((bits & 1u) != 0u)
    {
        vec4 t = floor(texture(uTexture, vSTQ.xy / vSTQ.z) * 255.0 + 0.5);
        bool tcc = (bits & 2u) != 0u;
        uint tfx = (bits >> 2) & 3u;
        if (tfx == 0u) // MODULATE
        {
            c.rgb = min(floor(t.rgb * v.rgb / 128.0), vec3(255.0));
            c.a = tcc ? min(floor(t.a * v.a / 128.0), 255.0) : v.a;
        }
        else if (tfx == 1u) // DECAL
        {
            c.rgb = t.rgb;
            c.a = tcc ? t.a : v.a;
        }
        else // HIGHLIGHT / HIGHLIGHT2
        {
            c.rgb = min(floor(t.rgb * v.rgb / 128.0) + v.a, vec3(255.0));
            c.a = tcc ? (tfx == 2u ? min(t.a + v.a, 255.0) : t.a) : v.a;
        }
    }
    if ((bits & 16u) != 0u && !alphaPasses(c.a, (bits >> 5) & 7u, pc.shade.x) && ((bits >> 8) & 3u) == 0u)
        discard; // AFAIL = KEEP; the other AFAIL modes still draw
    vec3 tint = unpackUnorm4x8(floatBitsToUint(pc.misc.y)).rgb;
    // Alpha leaves as As / 0x80 so blend factors see the GS's scale.
    // FBA sets the stored alpha's top bit, i.e. at least 0x80.
    float storedAlpha = (bits & 1024u) != 0u ? 1.0 : min(c.a / 128.0, 1.0);
    FragColor = vec4(mix(c.rgb / 255.0, tint, pc.misc.z), storedAlpha);
}
