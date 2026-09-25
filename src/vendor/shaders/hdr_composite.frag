#version 450
// The overlay, drawn in SDR as upstream draws it, laid over an HDR frame.
//
// The game's pixel is brought to SDR relative to the UI white, the overlay is
// blended over it in sRGB space exactly as it would be in an SDR swapchain,
// and the result goes back to the swapchain's encoding. Where the overlay
// drew nothing the game's pixel is left alone.

layout(set = 0, binding = 0) uniform sampler2D overlay;  // premultiplied, sRGB
layout(set = 0, binding = 1) uniform sampler2D game;     // the frame, as presented

layout(push_constant) uniform Params {
    float whiteNits;   // the level the game draws its UI white at
    int encoding;      // 0: HDR10 PQ, BT.2020; 1: scRGB, linear BT.709 at 80 nits
} params;

layout(location = 0) out vec4 outColor;

const mat3 kTo709 = mat3(1.6605, -0.1246, -0.0182,
                         -0.5876, 1.1329, -0.1006,
                         -0.0728, -0.0083, 1.1187);
const mat3 kTo2020 = mat3(0.6274, 0.0691, 0.0164,
                          0.3293, 0.9195, 0.0880,
                          0.0433, 0.0114, 0.8956);

const float m1 = 0.1593017578125, m2 = 78.84375;
const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;

vec3 pqToNits(vec3 e)
{
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1));
}

vec3 nitsToPq(vec3 nits)
{
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

vec3 srgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 linearToSrgb(vec3 l)
{
    return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, l));
}

void main()
{
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec4 o = texelFetch(overlay, at, 0);
    if (o.a <= 0.0) discard;

    vec4 g = texelFetch(game, at, 0);
    vec3 gameNits709 = params.encoding == 0 ? kTo709 * pqToNits(g.rgb) : g.rgb * 80.0;
    vec3 gameSdr = linearToSrgb(clamp(gameNits709 / params.whiteNits, 0.0, 1.0));

    vec3 blended = o.rgb + (1.0 - o.a) * gameSdr;
    vec3 nits709 = srgbToLinear(clamp(blended, 0.0, 1.0)) * params.whiteNits;

    if (params.encoding == 0) {
        outColor = vec4(nitsToPq(kTo2020 * nits709), 1.0);
    } else {
        outColor = vec4(nits709 / 80.0, 1.0);
    }
}
