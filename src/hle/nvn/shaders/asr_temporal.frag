#version 450
// Arm ASR-style temporal pass (the accumulate step of FSR 2 / Arm ASR, reduced to what this loader has: the game gives
// no depth, no motion vectors and no camera jitter, so the history is reprojected with zero motion and a moving
// object leaves a trail until the colour clip below rejects it: that is the ghosting).
//   uCur  = the EASU output at the screen's size (the current frame, already enlarged)
//   uHist = the previous frame's result of this pass
// The history is clipped to the colour box of the current pixel's neighbourhood (variance clipping in YCoCg, with the
// colours tone-compressed so HDR targets do not flicker), then blended: a clipped or very different history gets less
// weight, which is what keeps the trails short.
layout(set = 0, binding = 0) uniform sampler2D uCur;
layout(set = 0, binding = 1) uniform sampler2D uHist;
layout(push_constant) uniform PC {
    vec2 inv_out;   // 1 / output size
    float spacing;  // distance in output pixels between the neighbourhood taps
    float reset;    // > 0.5: no valid history, the current frame is copied
    float w_hist;   // history weight at rest, 0..1
    float pad0;
    float pad1;
    float pad2;
} pc;
layout(location = 0) out vec4 outColor;

vec3 compress(vec3 c) {
    c = max(c, vec3(0.0));
    return c / (1.0 + max(max(c.r, c.g), c.b));
}
vec3 decompress(vec3 c) {
    return c / max(1.0 - max(max(c.r, c.g), c.b), 1e-4);
}
vec3 toYcc(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 fromYcc(vec3 y) {
    float t = y.x - y.z;
    return vec3(t + y.y, y.x + y.z, t - y.y);
}
vec3 tap(vec2 uv) {
    return toYcc(compress(texture(uCur, uv).rgb));
}

void main() {
    vec2 uv = gl_FragCoord.xy * pc.inv_out;
    vec4 cur = texture(uCur, uv);
    if (pc.reset > 0.5) {
        outColor = cur;
        return;
    }
    vec2 d = pc.spacing * pc.inv_out;
    vec3 c0 = toYcc(compress(cur.rgb));
    vec3 s1 = c0, s2 = c0 * c0, lo = c0, hi = c0;
    const vec2 offs[8] = vec2[8](vec2(-1.0, -1.0), vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 0.0),
                                 vec2(1.0, 0.0), vec2(-1.0, 1.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
    for (int i = 0; i < 8; i++) {
        vec3 c = tap(uv + offs[i] * d);
        s1 += c;
        s2 += c * c;
        lo = min(lo, c);
        hi = max(hi, c);
    }
    vec3 mean = s1 / 9.0;
    vec3 sigma = sqrt(max(s2 / 9.0 - mean * mean, vec3(0.0)));
    const float gamma = 1.25;
    vec3 boxLo = max(mean - gamma * sigma, lo);
    vec3 boxHi = min(mean + gamma * sigma, hi);

    vec3 h = toYcc(compress(texture(uHist, uv).rgb));
    vec3 center = 0.5 * (boxLo + boxHi);
    vec3 ext = 0.5 * (boxHi - boxLo) + vec3(1e-4);
    vec3 v = h - center;
    vec3 a = abs(v / ext);
    float m = max(a.x, max(a.y, a.z));
    vec3 hc = m > 1.0 ? center + v / m : h;

    float rejected = clamp(length(h - hc) * 8.0, 0.0, 1.0);
    float lumaDiff = abs(hc.x - c0.x);
    float alpha = mix(1.0 - pc.w_hist, 1.0, clamp(rejected + lumaDiff * 3.0, 0.0, 1.0));
    vec3 res = mix(hc, c0, alpha);
    outColor = vec4(decompress(fromYcc(res)), cur.a);
}
